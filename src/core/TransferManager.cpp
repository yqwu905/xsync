#include "core/TransferManager.h"

#include "core/FileSystemBackend.h"
#include "core/MemoryStats.h"
#include "core/ProtocolNegotiator.h"
#include "core/SshClient.h"
#include "core/TransferWorker.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMetaObject>
#include <algorithm>
#include <chrono>

namespace xsync {

namespace {

struct ScannedFile {
    QString sourcePath;
    QString relativePath;
    QString itemName;
    QString relParentDir;
    uint64_t size = 0;
};

struct ScanOutcome {
    bool ok = false;
    QString error;
    std::vector<QString> subdirs;
    std::vector<ScannedFile> files;
};

bool scanRemoteDirTree(SshClient& client,
                       const QString& rootRemote,
                       const QString& relPrefix,
                       ScanOutcome& out,
                       const std::atomic<bool>& cancelFlag) {
    if (cancelFlag.load(std::memory_order_relaxed)) {
        out.error = QStringLiteral("Canceled");
        return false;
    }
    std::vector<FileEntry> entries;
    QString err;
    if (!client.listDirectory(rootRemote, entries, err, &cancelFlag)) {
        out.error = err;
        return false;
    }
    for (const FileEntry& e : entries) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            out.error = QStringLiteral("Canceled");
            return false;
        }
        const QString rel = relPrefix.isEmpty() ? e.name : (relPrefix + QLatin1Char('/') + e.name);
        if (e.isDir && !e.isSymlink) {
            out.subdirs.push_back(rel);
            if (!scanRemoteDirTree(client, e.fullPath, rel, out, cancelFlag)) {
                return false;
            }
        } else {
            ScannedFile sf;
            sf.sourcePath = e.fullPath;
            sf.relativePath = rel;
            sf.itemName = e.name;
            sf.relParentDir = relPrefix;
            sf.size = e.size;
            out.files.push_back(std::move(sf));
        }
    }
    return true;
}

}  // namespace

TransferManager::TransferManager(QObject* parent) : QObject(parent) {}

TransferManager::~TransferManager() {
    shutdown();
}

void TransferManager::shutdown() {
    shuttingDown_.store(true, std::memory_order_release);

    for (auto it = runningControls_.begin(); it != runningControls_.end(); ++it) {
        if (it.value().cancelFlag) it.value().cancelFlag->store(true, std::memory_order_relaxed);
    }
    for (auto& sw : scanWorkers_) {
        if (sw.cancelFlag) sw.cancelFlag->store(true, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(workMutex_);
        readyQueue_.clear();
    }
    workCv_.notify_all();

    for (auto& w : workerThreads_) {
        if (w.thread.joinable()) w.thread.join();
    }
    workerThreads_.clear();

    for (auto& sw : scanWorkers_) {
        if (sw.thread.joinable()) sw.thread.join();
    }
    scanWorkers_.clear();
    runningControls_.clear();
}

bool TransferManager::isTaskTerminal(TransferStatus st) {
    return st == TransferStatus::Completed || st == TransferStatus::Failed || st == TransferStatus::Canceled;
}

bool TransferManager::isBatchTerminal(const TransferBatch& b) {
    if (b.isScanning) return false;
    return isTaskTerminal(b.status);
}

int TransferManager::indexOfTask(uint64_t taskId) const {
    for (size_t i = 0; i < tasks_.size(); ++i) {
        if (tasks_[i].id == taskId) return static_cast<int>(i);
    }
    return -1;
}

int TransferManager::indexOfBatch(uint64_t batchId) const {
    for (size_t i = 0; i < batches_.size(); ++i) {
        if (batches_[i].id == batchId) return static_cast<int>(i);
    }
    return -1;
}

const TransferBatch* TransferManager::findBatch(uint64_t batchId) const {
    int idx = indexOfBatch(batchId);
    return idx >= 0 ? &batches_[static_cast<size_t>(idx)] : nullptr;
}

const TransferTask* TransferManager::findTask(uint64_t taskId) const {
    int idx = indexOfTask(taskId);
    return idx >= 0 ? &tasks_[static_cast<size_t>(idx)] : nullptr;
}

std::vector<TransferTask> TransferManager::tasksInBatch(uint64_t batchId) const {
    std::vector<TransferTask> out;
    for (const TransferTask& t : tasks_) {
        if (t.batchId == batchId) out.push_back(t);
    }
    return out;
}

void TransferManager::cleanupExitedWorkers() {
    for (auto it = workerThreads_.begin(); it != workerThreads_.end();) {
        if (it->exited && it->exited->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) it->thread.join();
            it = workerThreads_.erase(it);
        } else {
            ++it;
        }
    }
}

void TransferManager::cleanupFinishedScanners() {
    for (auto it = scanWorkers_.begin(); it != scanWorkers_.end();) {
        if (it->doneFlag && it->doneFlag->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) it->thread.join();
            it = scanWorkers_.erase(it);
        } else {
            ++it;
        }
    }
}

uint64_t TransferManager::enqueueTask(TransferTask task) {
    if (task.itemName.isEmpty()) {
        task.itemName = baseNameAnyPath(task.sourcePath);
    }
    if (task.isDir) {
        return enqueueDirectoryBatch(std::move(task));
    }

    task.id = nextTaskId_++;
    task.batchId = 0;
    task.status = TransferStatus::Queued;
    task.progressPercent = 0;
    task.transferredBytes = 0;
    task.speedBytesPerSec = 0.0;
    task.errorMessage.clear();
    task.createdAt = QDateTime::currentDateTime();
    task.statusMessage = QStringLiteral("Queued");

    tasks_.push_back(task);
    emit taskAdded(task);
    emit queueStatsChanged();
    scheduleNext();
    return task.id;
}

uint64_t TransferManager::enqueueDirectoryBatch(TransferTask dirTask) {
    TransferBatch batch;
    batch.id = nextBatchId_++;
    batch.sourceEndpoint = dirTask.sourceEndpoint;
    batch.destEndpoint = dirTask.destEndpoint;
    batch.sourcePath = dirTask.sourcePath;
    batch.destDir = dirTask.destDir;
    batch.itemName = dirTask.itemName.isEmpty() ? baseNameAnyPath(dirTask.sourcePath) : dirTask.itemName;
    batch.deleteSourceAfter = dirTask.deleteSourceAfter;
    batch.isScanning = true;
    batch.status = TransferStatus::Scanning;
    batch.statusMessage = QStringLiteral("Scanning directory...");
    batch.createdAt = QDateTime::currentDateTime();
    batch.targetPaneId = dirTask.targetPaneId;

    batches_.push_back(batch);
    emit batchAdded(batch);
    emit queueStatsChanged();

    startDirectoryScan(batch, dirTask.protocolPreference, dirTask.r2rPolicy);
    return batch.id;
}

void TransferManager::startDirectoryScan(const TransferBatch& batch,
                                         ProtocolPreference protoPref,
                                         RemoteToRemotePolicy r2rPolicy) {
    cleanupFinishedScanners();

    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto doneFlag = std::make_shared<std::atomic<bool>>(false);

    ScanWorkerHandle handle;
    handle.batchId = batch.id;
    handle.cancelFlag = cancelFlag;
    handle.doneFlag = doneFlag;

    handle.thread = std::thread([this, batch, protoPref, r2rPolicy, cancelFlag, doneFlag]() {
        ScanOutcome outcome;

        if (batch.sourceEndpoint.isLocal()) {
            QFileInfo rootInfo(batch.sourcePath);
            if (!rootInfo.exists() || !rootInfo.isDir()) {
                outcome.error = QStringLiteral("Source directory not found: %1").arg(batch.sourcePath);
            } else {
                const QDir baseDir(batch.sourcePath);
                QDirIterator it(batch.sourcePath,
                                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                QDirIterator::Subdirectories);
                bool canceled = false;
                while (it.hasNext()) {
                    if (cancelFlag->load(std::memory_order_relaxed)) {
                        canceled = true;
                        outcome.error = QStringLiteral("Canceled");
                        break;
                    }
                    it.next();
                    const QFileInfo fi = it.fileInfo();
                    const QString rel = baseDir.relativeFilePath(fi.absoluteFilePath());
                    if (fi.isDir() && !fi.isSymLink()) {
                        outcome.subdirs.push_back(rel);
                    } else {
                        ScannedFile sf;
                        sf.sourcePath = fi.absoluteFilePath();
                        sf.relativePath = rel;
                        sf.itemName = fi.fileName();
                        const int slash = rel.lastIndexOf(QLatin1Char('/'));
                        sf.relParentDir = (slash >= 0) ? rel.left(slash) : QString();
                        sf.size = static_cast<uint64_t>(std::max<qint64>(0, fi.size()));
                        outcome.files.push_back(std::move(sf));
                    }
                }
                outcome.ok = !canceled;
            }
        } else {
            SshClient srcClient;
            QString err;
            if (!srcClient.connectToHost(batch.sourceEndpoint.session, err, cancelFlag.get())) {
                outcome.error = QStringLiteral("Scan connect failed: %1").arg(err);
            } else {
                outcome.ok = scanRemoteDirTree(srcClient, batch.sourcePath, {}, outcome, *cancelFlag);
            }
        }

        // Pre-create target root directory + empty subdirectories so even empty folders are preserved
        if (outcome.ok && !cancelFlag->load(std::memory_order_relaxed)) {
            if (batch.destEndpoint.isLocal()) {
                QString mkErr;
                const QString dstRoot = LocalFileSystem::joinPath(batch.destDir, batch.itemName);
                LocalFileSystem::mkdir(dstRoot, mkErr);
                for (const QString& sub : outcome.subdirs) {
                    LocalFileSystem::mkdir(LocalFileSystem::joinPath(dstRoot, sub), mkErr);
                }
            } else {
                SshClient dstClient;
                QString err;
                if (dstClient.connectToHost(batch.destEndpoint.session, err, cancelFlag.get())) {
                    const QString dstRoot = joinRemotePath(batch.destDir, batch.itemName);
                    dstClient.mkdirRecursive(dstRoot, err, cancelFlag.get());
                    for (const QString& sub : outcome.subdirs) {
                        if (cancelFlag->load(std::memory_order_relaxed)) break;
                        dstClient.mkdirRecursive(joinRemotePath(dstRoot, sub), err, cancelFlag.get());
                    }
                }
            }
        }

        doneFlag->store(true, std::memory_order_release);

        if (shuttingDown_.load(std::memory_order_acquire)) return;

        auto sharedOutcome = std::make_shared<ScanOutcome>(std::move(outcome));
        QMetaObject::invokeMethod(
            this,
            [this, batchId = batch.id, protoPref, r2rPolicy, cancelFlag, sharedOutcome]() {
                cleanupFinishedScanners();
                int bIdx = indexOfBatch(batchId);
                if (bIdx < 0) return;  // Batch was removed while scanning

                TransferBatch& b = batches_[static_cast<size_t>(bIdx)];
                b.isScanning = false;

                if (cancelFlag->load(std::memory_order_relaxed)) {
                    b.status = TransferStatus::Canceled;
                    b.statusMessage = QStringLiteral("Canceled");
                    b.finishedAt = QDateTime::currentDateTime();
                    emit batchUpdated(b);
                    enforceHistoryBound();
                    emit queueStatsChanged();
                    return;
                }

                if (!sharedOutcome->ok) {
                    b.status = TransferStatus::Failed;
                    b.errorMessage = sharedOutcome->error;
                    b.statusMessage = QStringLiteral("Scan failed: %1").arg(sharedOutcome->error);
                    b.finishedAt = QDateTime::currentDateTime();
                    emit batchUpdated(b);
                    emit logMessage(QStringLiteral("[Folder #%1] Failed to scan '%2': %3")
                                        .arg(b.id)
                                        .arg(b.itemName, sharedOutcome->error));
                    enforceHistoryBound();
                    emit queueStatsChanged();
                    return;
                }

                if (sharedOutcome->files.empty()) {
                    b.totalFiles = 0;
                    b.completedFiles = 0;
                    b.progressPercent = 100;
                    b.status = TransferStatus::Completed;
                    b.statusMessage = QStringLiteral("Completed (empty folder)");
                    b.finishedAt = QDateTime::currentDateTime();
                    emit batchUpdated(b);
                    if (!b.targetPaneId.isEmpty()) emit paneRefreshRequested(b.targetPaneId);
                    enforceHistoryBound();
                    emit queueStatsChanged();
                    return;
                }

                const bool startPaused = batchPausedByUser_.value(batchId, false);
                const QString dstRoot = b.destEndpoint.isLocal()
                                            ? LocalFileSystem::joinPath(b.destDir, b.itemName)
                                            : joinRemotePath(b.destDir, b.itemName);

                tasks_.reserve(tasks_.size() + sharedOutcome->files.size());
                for (const ScannedFile& sf : sharedOutcome->files) {
                    TransferTask child;
                    child.id = nextTaskId_++;
                    child.batchId = batchId;
                    child.sourceEndpoint = b.sourceEndpoint;
                    child.destEndpoint = b.destEndpoint;
                    child.sourcePath = sf.sourcePath;
                    if (sf.relParentDir.isEmpty()) {
                        child.destDir = dstRoot;
                    } else {
                        child.destDir = b.destEndpoint.isLocal()
                                            ? LocalFileSystem::joinPath(dstRoot, sf.relParentDir)
                                            : joinRemotePath(dstRoot, sf.relParentDir);
                    }
                    child.itemName = sf.itemName;
                    child.relativePath = sf.relativePath;
                    child.isDir = false;
                    child.deleteSourceAfter = false;  // Handled at batch completion only if 100% succeeded
                    child.totalBytes = sf.size;
                    child.protocolPreference = protoPref;
                    child.r2rPolicy = r2rPolicy;
                    child.status = startPaused ? TransferStatus::Paused : TransferStatus::Queued;
                    child.statusMessage = startPaused ? QStringLiteral("Paused") : QStringLiteral("Queued");
                    child.createdAt = QDateTime::currentDateTime();

                    tasks_.push_back(child);
                    emit taskAdded(child);
                }

                emit logMessage(QStringLiteral("[Folder #%1] Expanded '%2': %3 files (%4)")
                                    .arg(b.id)
                                    .arg(b.itemName)
                                    .arg(sharedOutcome->files.size())
                                    .arg(formatBytes([&]() {
                                        uint64_t sum = 0;
                                        for (const auto& f : sharedOutcome->files) sum += f.size;
                                        return sum;
                                    }())));

                recomputeBatchStats(batchId);
                emit queueStatsChanged();
                scheduleNext();
            },
            Qt::QueuedConnection);
    });

    scanWorkers_.push_back(std::move(handle));
}

void TransferManager::recomputeBatchStats(uint64_t batchId, bool allowSourceDelete) {
    int bIdx = indexOfBatch(batchId);
    if (bIdx < 0) return;

    TransferBatch& b = batches_[static_cast<size_t>(bIdx)];
    if (b.isScanning) return;

    int total = 0;
    int completed = 0;
    int failed = 0;
    int paused = 0;
    int canceled = 0;
    int active = 0;
    int queued = 0;
    uint64_t totalBytes = 0;
    uint64_t xferBytes = 0;
    double speed = 0.0;
    QString firstErr;
    TransferProtocol lastActiveProto = b.activeProtocol;

    for (const TransferTask& t : tasks_) {
        if (t.batchId != batchId) continue;
        ++total;
        totalBytes += t.totalBytes;
        xferBytes += (t.status == TransferStatus::Completed)
                         ? std::max(t.totalBytes, t.transferredBytes)
                         : t.transferredBytes;
        switch (t.status) {
            case TransferStatus::Completed:
                ++completed;
                lastActiveProto = t.activeProtocol;
                break;
            case TransferStatus::Failed:
                ++failed;
                if (firstErr.isEmpty()) firstErr = t.errorMessage.isEmpty() ? t.statusMessage : t.errorMessage;
                break;
            case TransferStatus::Paused:
                ++paused;
                break;
            case TransferStatus::Canceled:
                ++canceled;
                break;
            case TransferStatus::Running:
            case TransferStatus::Negotiating:
                ++active;
                speed += t.speedBytesPerSec;
                lastActiveProto = t.activeProtocol;
                break;
            case TransferStatus::Queued:
                ++queued;
                break;
            default:
                break;
        }
    }

    b.totalFiles = total;
    b.completedFiles = completed;
    b.failedFiles = failed;
    b.pausedFiles = paused;
    b.canceledFiles = canceled;
    b.activeFiles = active;
    b.totalBytes = totalBytes;
    b.transferredBytes = xferBytes;
    b.speedBytesPerSec = speed;
    b.activeProtocol = lastActiveProto;
    b.errorMessage = firstErr;

    if (totalBytes > 0) {
        b.progressPercent = static_cast<int>(std::min<uint64_t>(100, (xferBytes * 100) / totalBytes));
    } else if (total > 0) {
        b.progressPercent = (completed * 100) / total;
    } else {
        b.progressPercent = 100;
    }

    const TransferStatus prevStatus = b.status;
    if (active > 0) {
        b.status = TransferStatus::Running;
        b.statusMessage = failed > 0
                              ? QStringLiteral("Transferring (%1/%2 files, %3 failed)")
                                    .arg(completed).arg(total).arg(failed)
                              : QStringLiteral("Transferring (%1/%2 files)")
                                    .arg(completed).arg(total);
    } else if (queued > 0) {
        b.status = TransferStatus::Queued;
        b.statusMessage = QStringLiteral("Queued (%1/%2 files)").arg(completed).arg(total);
    } else if (paused > 0) {
        b.status = TransferStatus::Paused;
        b.statusMessage = QStringLiteral("Paused (%1/%2 done, %3 paused)")
                              .arg(completed).arg(total).arg(paused);
    } else if (failed > 0) {
        b.status = TransferStatus::Failed;
        b.statusMessage = QStringLiteral("%1 failed / %2 files").arg(failed).arg(total);
        if (!b.finishedAt.isValid()) b.finishedAt = QDateTime::currentDateTime();
    } else if (canceled > 0 && completed < total) {
        b.status = TransferStatus::Canceled;
        b.statusMessage = QStringLiteral("Canceled (%1/%2 completed)").arg(completed).arg(total);
        if (!b.finishedAt.isValid()) b.finishedAt = QDateTime::currentDateTime();
    } else {
        b.status = TransferStatus::Completed;
        b.progressPercent = 100;
        b.statusMessage = QStringLiteral("Completed (%1 files)").arg(completed);
        if (!b.finishedAt.isValid()) b.finishedAt = QDateTime::currentDateTime();
    }

    emit batchUpdated(b);

    // Refresh target pane once when batch transitions into terminal state
    if (!isTaskTerminal(prevStatus) && isTaskTerminal(b.status)) {
        if (!b.targetPaneId.isEmpty()) {
            emit paneRefreshRequested(b.targetPaneId);
        }
        // Only delete Cut source folder when 100% of files succeeded with 0 removals/errors
        if (allowSourceDelete && b.deleteSourceAfter && b.status == TransferStatus::Completed &&
            failed == 0 && canceled == 0 && paused == 0 && !batchHadManualRemoval_.value(batchId, false)) {
            const EndpointSpec srcEp = b.sourceEndpoint;
            const QString srcPath = b.sourcePath;
            if (srcEp.isLocal()) {
                QString delErr;
                LocalFileSystem::removePath(srcPath, delErr);
            } else {
                std::thread([srcEp, srcPath]() {
                    SshClient c;
                    QString err;
                    if (c.connectToHost(srcEp.session, err)) {
                        c.removeDirRecursive(srcPath, err);
                    }
                }).detach();
            }
        }
        enforceHistoryBound();
    }
}

void TransferManager::pauseTask(uint64_t taskId) {
    if (runningControls_.contains(taskId)) {
        const TaskControl& ctrl = runningControls_[taskId];
        if (ctrl.pauseFlag) ctrl.pauseFlag->store(true, std::memory_order_relaxed);
        if (ctrl.cancelFlag) ctrl.cancelFlag->store(true, std::memory_order_relaxed);
    }
    int idx = indexOfTask(taskId);
    if (idx < 0) return;
    TransferTask& t = tasks_[static_cast<size_t>(idx)];
    if (t.status == TransferStatus::Queued) {
        t.status = TransferStatus::Paused;
        t.statusMessage = QStringLiteral("Paused");
        t.speedBytesPerSec = 0.0;
        emit taskUpdated(t);
        if (t.batchId > 0) recomputeBatchStats(t.batchId);
        emit queueStatsChanged();
    }
}

void TransferManager::resumeTask(uint64_t taskId) {
    int idx = indexOfTask(taskId);
    if (idx < 0) return;
    TransferTask& t = tasks_[static_cast<size_t>(idx)];
    if (t.status != TransferStatus::Paused) return;

    t.status = TransferStatus::Queued;
    t.statusMessage = QStringLiteral("Queued");
    emit taskUpdated(t);
    if (t.batchId > 0) {
        batchPausedByUser_.remove(t.batchId);
        recomputeBatchStats(t.batchId);
    }
    emit queueStatsChanged();
    scheduleNext();
}

void TransferManager::cancelTask(uint64_t taskId) {
    if (runningControls_.contains(taskId)) {
        const TaskControl& ctrl = runningControls_[taskId];
        if (ctrl.pauseFlag) ctrl.pauseFlag->store(false, std::memory_order_relaxed);
        if (ctrl.cancelFlag) ctrl.cancelFlag->store(true, std::memory_order_relaxed);
    }
    int idx = indexOfTask(taskId);
    if (idx < 0) return;
    TransferTask& t = tasks_[static_cast<size_t>(idx)];
    if (t.status == TransferStatus::Queued || t.status == TransferStatus::Paused) {
        t.status = TransferStatus::Canceled;
        t.statusMessage = QStringLiteral("Canceled");
        t.speedBytesPerSec = 0.0;
        t.finishedAt = QDateTime::currentDateTime();
        emit taskUpdated(t);
        if (t.batchId > 0) {
            recomputeBatchStats(t.batchId);
        } else {
            enforceHistoryBound();
        }
        emit queueStatsChanged();
    }
}

void TransferManager::removeTask(uint64_t taskId) {
    if (runningControls_.contains(taskId)) {
        const TaskControl& ctrl = runningControls_[taskId];
        if (ctrl.pauseFlag) ctrl.pauseFlag->store(false, std::memory_order_relaxed);
        if (ctrl.cancelFlag) ctrl.cancelFlag->store(true, std::memory_order_relaxed);
    }
    int idx = indexOfTask(taskId);
    if (idx < 0) return;

    const uint64_t batchId = tasks_[static_cast<size_t>(idx)].batchId;
    tasks_.erase(tasks_.begin() + idx);
    emit taskRemoved(taskId);

    if (batchId > 0) {
        batchHadManualRemoval_.insert(batchId, true);
        // Check if any children remain in this batch
        bool anyLeft = false;
        for (const TransferTask& t : tasks_) {
            if (t.batchId == batchId) {
                anyLeft = true;
                break;
            }
        }
        int bIdx = indexOfBatch(batchId);
        if (bIdx >= 0 && !anyLeft && !batches_[static_cast<size_t>(bIdx)].isScanning) {
            batches_.erase(batches_.begin() + bIdx);
            batchPausedByUser_.remove(batchId);
            batchHadManualRemoval_.remove(batchId);
            emit batchRemoved(batchId);
        } else {
            recomputeBatchStats(batchId);
        }
    }
    emit queueStatsChanged();
}

void TransferManager::retryTask(uint64_t taskId) {
    int idx = indexOfTask(taskId);
    if (idx < 0) return;
    TransferTask& t = tasks_[static_cast<size_t>(idx)];
    if (t.status != TransferStatus::Failed && t.status != TransferStatus::Canceled) return;

    t.status = TransferStatus::Queued;
    t.progressPercent = 0;
    t.transferredBytes = 0;
    t.speedBytesPerSec = 0.0;
    t.errorMessage.clear();
    t.statusMessage = QStringLiteral("Queued (retry)");
    t.finishedAt = QDateTime();
    emit taskUpdated(t);
    if (t.batchId > 0) {
        int bIdx = indexOfBatch(t.batchId);
        if (bIdx >= 0) batches_[static_cast<size_t>(bIdx)].finishedAt = QDateTime();
        recomputeBatchStats(t.batchId);
    }
    emit queueStatsChanged();
    scheduleNext();
}

void TransferManager::pauseBatch(uint64_t batchId) {
    batchPausedByUser_.insert(batchId, true);
    std::vector<uint64_t> ids;
    for (const TransferTask& t : tasks_) {
        if (t.batchId == batchId &&
            (t.status == TransferStatus::Queued || t.status == TransferStatus::Running ||
             t.status == TransferStatus::Negotiating)) {
            ids.push_back(t.id);
        }
    }
    for (uint64_t id : ids) pauseTask(id);
    recomputeBatchStats(batchId);
}

void TransferManager::resumeBatch(uint64_t batchId) {
    batchPausedByUser_.remove(batchId);
    std::vector<uint64_t> ids;
    for (const TransferTask& t : tasks_) {
        if (t.batchId == batchId && t.status == TransferStatus::Paused) {
            ids.push_back(t.id);
        }
    }
    for (uint64_t id : ids) resumeTask(id);
    recomputeBatchStats(batchId);
}

void TransferManager::cancelBatch(uint64_t batchId) {
    for (auto& sw : scanWorkers_) {
        if (sw.batchId == batchId && sw.cancelFlag) {
            sw.cancelFlag->store(true, std::memory_order_relaxed);
        }
    }
    std::vector<uint64_t> ids;
    for (const TransferTask& t : tasks_) {
        if (t.batchId == batchId && !isTaskTerminal(t.status)) {
            ids.push_back(t.id);
        }
    }
    for (uint64_t id : ids) cancelTask(id);
    recomputeBatchStats(batchId);
}

void TransferManager::removeBatch(uint64_t batchId) {
    for (auto& sw : scanWorkers_) {
        if (sw.batchId == batchId && sw.cancelFlag) {
            sw.cancelFlag->store(true, std::memory_order_relaxed);
        }
    }
    // Cancel all running children & remove them
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->batchId == batchId) {
            const uint64_t id = it->id;
            if (runningControls_.contains(id)) {
                if (runningControls_[id].pauseFlag) runningControls_[id].pauseFlag->store(false, std::memory_order_relaxed);
                if (runningControls_[id].cancelFlag) runningControls_[id].cancelFlag->store(true, std::memory_order_relaxed);
            }
            it = tasks_.erase(it);
            emit taskRemoved(id);
        } else {
            ++it;
        }
    }
    int bIdx = indexOfBatch(batchId);
    if (bIdx >= 0) {
        batches_.erase(batches_.begin() + bIdx);
        batchPausedByUser_.remove(batchId);
        batchHadManualRemoval_.remove(batchId);
        emit batchRemoved(batchId);
    }
    emit queueStatsChanged();
}

void TransferManager::retryFailedInBatch(uint64_t batchId) {
    std::vector<uint64_t> ids;
    for (const TransferTask& t : tasks_) {
        if (t.batchId == batchId &&
            (t.status == TransferStatus::Failed || t.status == TransferStatus::Canceled)) {
            ids.push_back(t.id);
        }
    }
    for (uint64_t id : ids) retryTask(id);
}

void TransferManager::cancelAll() {
    for (auto& sw : scanWorkers_) {
        if (sw.cancelFlag) sw.cancelFlag->store(true, std::memory_order_relaxed);
    }
    for (auto it = runningControls_.begin(); it != runningControls_.end(); ++it) {
        if (it.value().pauseFlag) it.value().pauseFlag->store(false, std::memory_order_relaxed);
        if (it.value().cancelFlag) it.value().cancelFlag->store(true, std::memory_order_relaxed);
    }
    for (TransferTask& t : tasks_) {
        if (t.status == TransferStatus::Queued || t.status == TransferStatus::Paused) {
            t.status = TransferStatus::Canceled;
            t.statusMessage = QStringLiteral("Canceled");
            t.finishedAt = QDateTime::currentDateTime();
            emit taskUpdated(t);
        }
    }
    for (const TransferBatch& b : batches_) {
        recomputeBatchStats(b.id);
    }
    enforceHistoryBound();
    emit queueStatsChanged();
}

void TransferManager::retryAllFailed() {
    std::vector<uint64_t> ids;
    for (const TransferTask& t : tasks_) {
        if (t.status == TransferStatus::Failed) {
            ids.push_back(t.id);
        }
    }
    for (uint64_t id : ids) retryTask(id);
}

void TransferManager::clearFinished() {
    bool removedAny = false;

    // 1. Remove finished batches along with all their children
    std::vector<uint64_t> finishedBatchIds;
    for (const TransferBatch& b : batches_) {
        if (isBatchTerminal(b)) {
            finishedBatchIds.push_back(b.id);
        }
    }
    for (uint64_t bid : finishedBatchIds) {
        removeBatch(bid);
        removedAny = true;
    }

    // 2. Remove finished standalone tasks
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->batchId == 0 && isTaskTerminal(it->status)) {
            const uint64_t id = it->id;
            it = tasks_.erase(it);
            emit taskRemoved(id);
            removedAny = true;
        } else {
            ++it;
        }
    }

    if (tasks_.capacity() > 1024 && tasks_.size() < 128) {
        tasks_.shrink_to_fit();
        MemoryStats::trimProcessMemory();
    }
    if (removedAny) emit queueStatsChanged();
}

void TransferManager::setPaused(bool paused) {
    paused_ = paused;
    emit queueStatsChanged();
    if (!paused_) scheduleNext();
}

void TransferManager::setMaxConcurrent(int n) {
    maxConcurrent_ = qBound(1, n, 8);
    scheduleNext();
}

void TransferManager::setMaxHistorySize(int maxFinished) {
    maxHistory_ = std::max(10, maxFinished);
    enforceHistoryBound();
}

void TransferManager::cacheCapabilities(const EndpointSpec& ep, const EndpointCapabilities& caps) {
    std::lock_guard<std::mutex> lock(capMutex_);
    if (capCache_.size() > 128) {
        capCache_.clear();
    }
    capCache_.insert(ep.cacheKey(), caps);
}

EndpointCapabilities TransferManager::cachedCapabilities(const EndpointSpec& ep) const {
    std::lock_guard<std::mutex> lock(capMutex_);
    return capCache_.value(ep.cacheKey());
}

void TransferManager::clearCapabilityCache() {
    std::lock_guard<std::mutex> lock(capMutex_);
    capCache_.clear();
}

int TransferManager::activeCount() const {
    int count = 0;
    for (const TransferTask& t : tasks_) {
        if (t.status == TransferStatus::Running || t.status == TransferStatus::Negotiating) {
            ++count;
        }
    }
    for (const TransferBatch& b : batches_) {
        if (b.isScanning) ++count;
    }
    return count;
}

int TransferManager::queuedCount() const {
    int count = 0;
    for (const TransferTask& t : tasks_) {
        if (t.status == TransferStatus::Queued) ++count;
    }
    return count;
}

int TransferManager::failedCount() const {
    int count = 0;
    for (const TransferTask& t : tasks_) {
        if (t.status == TransferStatus::Failed) ++count;
    }
    return count;
}

double TransferManager::aggregateSpeedBytesPerSec() const {
    double sum = 0.0;
    for (const TransferTask& t : tasks_) {
        if (t.status == TransferStatus::Running) {
            sum += t.speedBytesPerSec;
        }
    }
    return sum;
}

void TransferManager::enforceHistoryBound() {
    // Count finished standalone tasks + finished batches
    int finishedStandalone = 0;
    for (const TransferTask& t : tasks_) {
        if (t.batchId == 0 && isTaskTerminal(t.status)) ++finishedStandalone;
    }
    int finishedBatches = 0;
    for (const TransferBatch& b : batches_) {
        if (isBatchTerminal(b)) ++finishedBatches;
    }

    while (finishedStandalone + finishedBatches > maxHistory_) {
        // Evict oldest finished standalone task first, otherwise oldest finished batch
        auto tIt = std::find_if(tasks_.begin(), tasks_.end(), [](const TransferTask& t) {
            return t.batchId == 0 && isTaskTerminal(t.status);
        });
        if (tIt != tasks_.end()) {
            const uint64_t id = tIt->id;
            tasks_.erase(tIt);
            --finishedStandalone;
            emit taskRemoved(id);
            continue;
        }
        auto bIt = std::find_if(batches_.begin(), batches_.end(), [](const TransferBatch& b) {
            return isBatchTerminal(b);
        });
        if (bIt != batches_.end()) {
            const uint64_t bid = bIt->id;
            removeBatch(bid);
            --finishedBatches;
            continue;
        }
        break;
    }
}

void TransferManager::scheduleNext() {
    cleanupExitedWorkers();
    cleanupFinishedScanners();
    if (paused_ || shuttingDown_.load(std::memory_order_acquire)) return;

    while (inFlightCount_ < maxConcurrent_) {
        int nextIdx = -1;
        for (size_t i = 0; i < tasks_.size(); ++i) {
            if (tasks_[i].status == TransferStatus::Queued) {
                nextIdx = static_cast<int>(i);
                break;
            }
        }
        if (nextIdx < 0) break;

        TransferTask& t = tasks_[static_cast<size_t>(nextIdx)];
        t.status = TransferStatus::Negotiating;
        t.statusMessage = QStringLiteral("Starting...");
        t.errorMessage.clear();
        TransferTask taskCopy = t;

        TaskControl ctrl;
        ctrl.cancelFlag = std::make_shared<std::atomic<bool>>(false);
        ctrl.pauseFlag = std::make_shared<std::atomic<bool>>(false);
        runningControls_.insert(taskCopy.id, ctrl);
        ++inFlightCount_;

        emit taskUpdated(taskCopy);
        if (taskCopy.batchId > 0) recomputeBatchStats(taskCopy.batchId);
        emit queueStatsChanged();

        {
            std::lock_guard<std::mutex> lock(workMutex_);
            readyQueue_.push_back(DispatchedJob{std::move(taskCopy), std::move(ctrl)});
        }
        ensureWorkerCapacity();
        workCv_.notify_one();
    }
}

void TransferManager::ensureWorkerCapacity() {
    cleanupExitedWorkers();
    while (static_cast<int>(workerThreads_.size()) < maxConcurrent_ &&
           static_cast<int>(workerThreads_.size()) < inFlightCount_) {
        auto exited = std::make_shared<std::atomic<bool>>(false);
        WorkerThreadHandle h;
        h.exited = exited;
        h.thread = std::thread(&TransferManager::workerLoop, this, exited);
        workerThreads_.push_back(std::move(h));
    }
}

void TransferManager::workerLoop(std::shared_ptr<std::atomic<bool>> exitedFlag) {
    // Reusable SSH connection cache for this worker thread (destroyed automatically on exit)
    WorkerSessionCache sessionCache;

    while (!shuttingDown_.load(std::memory_order_acquire)) {
        DispatchedJob job;
        bool hasJob = false;
        {
            std::unique_lock<std::mutex> lock(workMutex_);
            workCv_.wait_for(lock, std::chrono::seconds(3), [this]() {
                return shuttingDown_.load(std::memory_order_acquire) || !readyQueue_.empty();
            });
            if (shuttingDown_.load(std::memory_order_acquire)) break;
            if (!readyQueue_.empty()) {
                job = std::move(readyQueue_.front());
                readyQueue_.pop_front();
                hasJob = true;
            }
        }

        if (!hasJob) {
            // Idle for 3s -> exit thread & release cached SSH connections immediately
            break;
        }

        TransferTask& taskCopy = job.task;
        auto cancelFlag = job.ctrl.cancelFlag;
        auto pauseFlag = job.ctrl.pauseFlag;

        TransferCallbacks cb;
        cb.getOrProbeCaps = [this, cancelFlag](const EndpointSpec& ep, SshClient* client) {
            EndpointCapabilities c = cachedCapabilities(ep);
            if (c.probed) return c;
            if (ep.isLocal()) {
                c = ProtocolNegotiator::probeLocal();
            } else if (client) {
                c = ProtocolNegotiator::probeRemote(*client, cancelFlag.get());
            }
            if (c.probed) cacheCapabilities(ep, c);
            return c;
        };

        cb.onState = [this, id = taskCopy.id, bid = taskCopy.batchId](
                         TransferStatus st, TransferProtocol proto, const QString& msg) {
            if (shuttingDown_.load(std::memory_order_acquire)) return;
            QMetaObject::invokeMethod(
                this,
                [this, id, bid, st, proto, msg]() {
                    int i = indexOfTask(id);
                    if (i < 0) return;
                    TransferTask& t = tasks_[static_cast<size_t>(i)];
                    t.status = st;
                    t.activeProtocol = proto;
                    t.statusMessage = msg;
                    emit taskUpdated(t);
                    if (bid > 0) recomputeBatchStats(bid);
                    emit queueStatsChanged();
                },
                Qt::QueuedConnection);
        };

        cb.onProgress = [this, id = taskCopy.id, bid = taskCopy.batchId](
                            uint64_t transferred, uint64_t total, int pct, double speed) {
            if (shuttingDown_.load(std::memory_order_acquire)) return;
            QMetaObject::invokeMethod(
                this,
                [this, id, bid, transferred, total, pct, speed]() {
                    int i = indexOfTask(id);
                    if (i < 0) return;
                    TransferTask& t = tasks_[static_cast<size_t>(i)];
                    t.transferredBytes = transferred;
                    t.totalBytes = total;
                    t.progressPercent = pct;
                    t.speedBytesPerSec = speed;
                    emit taskUpdated(t);
                    if (bid > 0) recomputeBatchStats(bid);
                    emit queueStatsChanged();
                },
                Qt::QueuedConnection);
        };

        cb.onLog = [this](const QString& line) {
            if (shuttingDown_.load(std::memory_order_acquire)) return;
            QMetaObject::invokeMethod(
                this, [this, line]() { emit logMessage(line); }, Qt::QueuedConnection);
        };

        QString err;
        const bool ok = TransferWorker::execute(taskCopy, *cancelFlag, cb, err, &sessionCache);
        const bool paused = pauseFlag->load(std::memory_order_relaxed);
        const bool canceled = cancelFlag->load(std::memory_order_relaxed);
        const TransferProtocol finalProto = taskCopy.activeProtocol;
        const QString targetPaneId = taskCopy.targetPaneId;
        const uint64_t batchId = taskCopy.batchId;

        if (shuttingDown_.load(std::memory_order_acquire)) break;

        QMetaObject::invokeMethod(
            this,
            [this, id = taskCopy.id, batchId, ok, paused, canceled, err, finalProto, targetPaneId]() {
                runningControls_.remove(id);
                if (inFlightCount_ > 0) --inFlightCount_;

                int i = indexOfTask(id);
                if (i >= 0) {
                    TransferTask& t = tasks_[static_cast<size_t>(i)];
                    t.activeProtocol = finalProto;
                    t.speedBytesPerSec = 0.0;
                    if (ok) {
                        t.status = TransferStatus::Completed;
                        t.progressPercent = 100;
                        t.transferredBytes = std::max(t.totalBytes, t.transferredBytes);
                        t.errorMessage.clear();
                        t.statusMessage = QStringLiteral("Done (%1)").arg(protocolToString(finalProto));
                        t.finishedAt = QDateTime::currentDateTime();
                        emit logMessage(QStringLiteral("[Task #%1] Completed '%2' via %3")
                                            .arg(t.id)
                                            .arg(t.relativePath.isEmpty() ? t.itemName : t.relativePath,
                                                 protocolToString(finalProto)));
                    } else if (paused) {
                        t.status = TransferStatus::Paused;
                        t.statusMessage = QStringLiteral("Paused");
                        t.errorMessage.clear();
                        emit logMessage(QStringLiteral("[Task #%1] Paused '%2'")
                                            .arg(t.id)
                                            .arg(t.relativePath.isEmpty() ? t.itemName : t.relativePath));
                    } else if (canceled) {
                        t.status = TransferStatus::Canceled;
                        t.statusMessage = QStringLiteral("Canceled");
                        t.errorMessage.clear();
                        t.finishedAt = QDateTime::currentDateTime();
                        emit logMessage(QStringLiteral("[Task #%1] Canceled '%2'")
                                            .arg(t.id)
                                            .arg(t.relativePath.isEmpty() ? t.itemName : t.relativePath));
                    } else {
                        t.status = TransferStatus::Failed;
                        t.errorMessage = err;
                        t.statusMessage = QStringLiteral("Error: %1").arg(err);
                        t.finishedAt = QDateTime::currentDateTime();
                        emit logMessage(QStringLiteral("[Task #%1] Failed '%2': %3")
                                            .arg(t.id)
                                            .arg(t.relativePath.isEmpty() ? t.itemName : t.relativePath, err));
                    }
                    emit taskUpdated(t);
                }

                if (batchId > 0) {
                    recomputeBatchStats(batchId, true);
                } else {
                    if (ok && !targetPaneId.isEmpty()) {
                        emit paneRefreshRequested(targetPaneId);
                    }
                    enforceHistoryBound();
                }

                emit queueStatsChanged();
                scheduleNext();
            },
            Qt::QueuedConnection);
    }

    sessionCache.clear();
    exitedFlag->store(true, std::memory_order_release);
}

}  // namespace xsync
