#include "core/TransferManager.h"

#include "core/MemoryStats.h"
#include "core/ProtocolNegotiator.h"
#include "core/TransferWorker.h"

#include <QMetaObject>
#include <algorithm>

namespace xsync {

TransferManager::TransferManager(QObject* parent) : QObject(parent) {}

TransferManager::~TransferManager() {
    shutdown();
}

void TransferManager::shutdown() {
    shuttingDown_ = true;
    for (auto& w : activeWorkers_) {
        if (w.cancelFlag) w.cancelFlag->store(true, std::memory_order_relaxed);
    }
    for (auto& w : activeWorkers_) {
        if (w.thread.joinable()) w.thread.join();
    }
    activeWorkers_.clear();
}

int TransferManager::indexOfTask(uint64_t taskId) const {
    for (size_t i = 0; i < tasks_.size(); ++i) {
        if (tasks_[i].id == taskId) return static_cast<int>(i);
    }
    return -1;
}

void TransferManager::cleanupFinishedThreads() {
    for (auto it = activeWorkers_.begin(); it != activeWorkers_.end();) {
        if (it->doneFlag && it->doneFlag->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) it->thread.join();
            it = activeWorkers_.erase(it);
        } else {
            ++it;
        }
    }
}

uint64_t TransferManager::enqueueTask(TransferTask task) {
    task.id = nextTaskId_++;
    task.status = TransferStatus::Queued;
    task.progressPercent = 0;
    task.transferredBytes = 0;
    task.speedBytesPerSec = 0.0;
    task.createdAt = QDateTime::currentDateTime();
    task.statusMessage = QStringLiteral("Queued");
    if (task.itemName.isEmpty()) {
        task.itemName = baseNameAnyPath(task.sourcePath);
    }

    tasks_.push_back(task);
    emit taskAdded(task);
    emit queueStatsChanged();
    scheduleNext();
    return task.id;
}

void TransferManager::cancelTask(uint64_t taskId) {
    for (auto& w : activeWorkers_) {
        if (w.taskId == taskId && w.cancelFlag) {
            w.cancelFlag->store(true, std::memory_order_relaxed);
            break;
        }
    }
    int idx = indexOfTask(taskId);
    if (idx >= 0) {
        TransferTask& t = tasks_[static_cast<size_t>(idx)];
        if (t.status == TransferStatus::Queued) {
            t.status = TransferStatus::Canceled;
            t.statusMessage = QStringLiteral("Canceled");
            t.finishedAt = QDateTime::currentDateTime();
            emit taskUpdated(t);
            enforceHistoryBound();
            emit queueStatsChanged();
        }
    }
}

void TransferManager::cancelAll() {
    for (auto& w : activeWorkers_) {
        if (w.cancelFlag) w.cancelFlag->store(true, std::memory_order_relaxed);
    }
    for (TransferTask& t : tasks_) {
        if (t.status == TransferStatus::Queued) {
            t.status = TransferStatus::Canceled;
            t.statusMessage = QStringLiteral("Canceled");
            t.finishedAt = QDateTime::currentDateTime();
            emit taskUpdated(t);
        }
    }
    enforceHistoryBound();
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
    t.statusMessage = QStringLiteral("Queued (retry)");
    t.finishedAt = QDateTime();
    emit taskUpdated(t);
    emit queueStatsChanged();
    scheduleNext();
}

void TransferManager::clearFinished() {
    bool removedAny = false;
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->status == TransferStatus::Completed ||
            it->status == TransferStatus::Failed ||
            it->status == TransferStatus::Canceled) {
            uint64_t id = it->id;
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
    return count;
}

int TransferManager::queuedCount() const {
    int count = 0;
    for (const TransferTask& t : tasks_) {
        if (t.status == TransferStatus::Queued) ++count;
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
    int finishedCount = 0;
    for (const TransferTask& t : tasks_) {
        if (t.status == TransferStatus::Completed ||
            t.status == TransferStatus::Failed ||
            t.status == TransferStatus::Canceled) {
            ++finishedCount;
        }
    }
    while (finishedCount > maxHistory_) {
        auto it = std::find_if(tasks_.begin(), tasks_.end(), [](const TransferTask& t) {
            return t.status == TransferStatus::Completed ||
                   t.status == TransferStatus::Failed ||
                   t.status == TransferStatus::Canceled;
        });
        if (it == tasks_.end()) break;
        const uint64_t id = it->id;
        tasks_.erase(it);
        --finishedCount;
        emit taskRemoved(id);
    }
}

void TransferManager::scheduleNext() {
    cleanupFinishedThreads();
    if (paused_ || shuttingDown_) return;

    while (static_cast<int>(activeWorkers_.size()) < maxConcurrent_) {
        uint64_t nextId = 0;
        for (const TransferTask& t : tasks_) {
            if (t.status == TransferStatus::Queued) {
                nextId = t.id;
                break;
            }
        }
        if (nextId == 0) break;
        startTask(nextId);
    }
}

void TransferManager::startTask(uint64_t taskId) {
    int idx = indexOfTask(taskId);
    if (idx < 0) return;

    tasks_[static_cast<size_t>(idx)].status = TransferStatus::Negotiating;
    tasks_[static_cast<size_t>(idx)].statusMessage = QStringLiteral("Starting...");
    TransferTask taskCopy = tasks_[static_cast<size_t>(idx)];
    emit taskUpdated(taskCopy);
    emit queueStatsChanged();

    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto doneFlag = std::make_shared<std::atomic<bool>>(false);

    RunningHandle handle;
    handle.taskId = taskId;
    handle.cancelFlag = cancelFlag;
    handle.doneFlag = doneFlag;

    handle.thread = std::thread([this, taskCopy, cancelFlag, doneFlag]() mutable {
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

        cb.onState = [this, id = taskCopy.id](TransferStatus st, TransferProtocol proto, const QString& msg) {
            QMetaObject::invokeMethod(
                this,
                [this, id, st, proto, msg]() {
                    int i = indexOfTask(id);
                    if (i < 0) return;
                    TransferTask& t = tasks_[static_cast<size_t>(i)];
                    t.status = st;
                    t.activeProtocol = proto;
                    t.statusMessage = msg;
                    emit taskUpdated(t);
                    emit queueStatsChanged();
                },
                Qt::QueuedConnection);
        };

        cb.onProgress = [this, id = taskCopy.id](uint64_t transferred, uint64_t total, int pct, double speed) {
            QMetaObject::invokeMethod(
                this,
                [this, id, transferred, total, pct, speed]() {
                    int i = indexOfTask(id);
                    if (i < 0) return;
                    TransferTask& t = tasks_[static_cast<size_t>(i)];
                    t.transferredBytes = transferred;
                    t.totalBytes = total;
                    t.progressPercent = pct;
                    t.speedBytesPerSec = speed;
                    emit taskUpdated(t);
                    emit queueStatsChanged();
                },
                Qt::QueuedConnection);
        };

        cb.onLog = [this](const QString& line) {
            QMetaObject::invokeMethod(
                this,
                [this, line]() { emit logMessage(line); },
                Qt::QueuedConnection);
        };

        QString err;
        const bool ok = TransferWorker::execute(taskCopy, *cancelFlag, cb, err);
        const bool canceled = cancelFlag->load(std::memory_order_relaxed);
        const TransferProtocol finalProto = taskCopy.activeProtocol;
        const QString targetPaneId = taskCopy.targetPaneId;

        doneFlag->store(true, std::memory_order_release);

        QMetaObject::invokeMethod(
            this,
            [this, id = taskCopy.id, ok, canceled, err, finalProto, targetPaneId]() {
                cleanupFinishedThreads();
                int i = indexOfTask(id);
                if (i >= 0) {
                    TransferTask& t = tasks_[static_cast<size_t>(i)];
                    t.activeProtocol = finalProto;
                    t.speedBytesPerSec = 0.0;
                    t.finishedAt = QDateTime::currentDateTime();
                    if (ok) {
                        t.status = TransferStatus::Completed;
                        t.progressPercent = 100;
                        t.statusMessage = QStringLiteral("Done (%1)").arg(protocolToString(finalProto));
                        emit logMessage(QStringLiteral("[Task #%1] Completed '%2' via %3")
                                            .arg(t.id)
                                            .arg(t.itemName, protocolToString(finalProto)));
                    } else if (canceled) {
                        t.status = TransferStatus::Canceled;
                        t.statusMessage = QStringLiteral("Canceled");
                        emit logMessage(QStringLiteral("[Task #%1] Canceled '%2'").arg(t.id).arg(t.itemName));
                    } else {
                        t.status = TransferStatus::Failed;
                        t.statusMessage = err;
                        emit logMessage(QStringLiteral("[Task #%1] Failed '%2': %3")
                                            .arg(t.id)
                                            .arg(t.itemName, err));
                    }
                    emit taskUpdated(t);
                }

                if (ok && !targetPaneId.isEmpty()) {
                    emit paneRefreshRequested(targetPaneId);
                }
                enforceHistoryBound();
                emit queueStatsChanged();
                scheduleNext();
            },
            Qt::QueuedConnection);
    });

    activeWorkers_.push_back(std::move(handle));
}

}  // namespace xsync
