#pragma once

#include "core/Types.h"

#include <QHash>
#include <QObject>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace xsync {

class SshClient;

// Thread-safe, bounded-concurrency transfer manager supporting:
// - Xftp-style directory expansion into folder TransferBatch + individual per-file TransferTasks
// - Selective pause / resume / cancel / delete / retry on individual files or entire folders
// - Per-worker SSH connection reuse (warm across files, auto-released on idle)
// - Strictly bounded completed history to guarantee zero memory leaks
class TransferManager : public QObject {
    Q_OBJECT
public:
    explicit TransferManager(QObject* parent = nullptr);
    ~TransferManager() override;

    // Enqueues a single file (standalone task) or a directory (auto-expanded TransferBatch).
    // Returns task ID for standalone files, or batch ID for directories.
    uint64_t enqueueTask(TransferTask task);

    // Individual file task controls (work on standalone files OR specific files inside a folder)
    void pauseTask(uint64_t taskId);
    void resumeTask(uint64_t taskId);
    void cancelTask(uint64_t taskId);
    void removeTask(uint64_t taskId);
    void retryTask(uint64_t taskId);

    // Folder batch controls
    void pauseBatch(uint64_t batchId);
    void resumeBatch(uint64_t batchId);
    void cancelBatch(uint64_t batchId);
    void removeBatch(uint64_t batchId);
    void retryFailedInBatch(uint64_t batchId);

    // Global queue controls
    void cancelAll();
    void retryAllFailed();
    void clearFinished();

    void setPaused(bool paused);
    bool isPaused() const { return paused_; }

    void setMaxConcurrent(int n);
    int maxConcurrent() const { return maxConcurrent_; }

    void setMaxHistorySize(int maxFinished);
    int maxHistorySize() const { return maxHistory_; }

    void cacheCapabilities(const EndpointSpec& ep, const EndpointCapabilities& caps);
    EndpointCapabilities cachedCapabilities(const EndpointSpec& ep) const;
    void clearCapabilityCache();

    const std::vector<TransferBatch>& batches() const { return batches_; }
    const std::vector<TransferTask>& tasks() const { return tasks_; }
    const TransferBatch* findBatch(uint64_t batchId) const;
    const TransferTask* findTask(uint64_t taskId) const;
    std::vector<TransferTask> tasksInBatch(uint64_t batchId) const;

    int activeCount() const;
    int queuedCount() const;
    int failedCount() const;
    double aggregateSpeedBytesPerSec() const;

    void shutdown();

signals:
    void batchAdded(const xsync::TransferBatch& batch);
    void batchUpdated(const xsync::TransferBatch& batch);
    void batchRemoved(uint64_t batchId);

    void taskAdded(const xsync::TransferTask& task);
    void taskUpdated(const xsync::TransferTask& task);
    void taskRemoved(uint64_t taskId);

    void queueStatsChanged();
    void logMessage(const QString& message);
    void paneRefreshRequested(const QString& paneId);

private:
    struct TaskControl {
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        std::shared_ptr<std::atomic<bool>> pauseFlag;
    };

    struct DispatchedJob {
        TransferTask task;
        TaskControl ctrl;
    };

    struct WorkerThreadHandle {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> exited;
    };

    struct ScanWorkerHandle {
        uint64_t batchId = 0;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        std::shared_ptr<std::atomic<bool>> doneFlag;
        std::thread thread;
    };

    uint64_t enqueueDirectoryBatch(TransferTask dirTask);
    void startDirectoryScan(const TransferBatch& batch,
                            ProtocolPreference protoPref,
                            RemoteToRemotePolicy r2rPolicy);
    void scheduleNext();
    void ensureWorkerCapacity();
    void workerLoop(std::shared_ptr<std::atomic<bool>> exitedFlag);
    void cleanupExitedWorkers();
    void cleanupFinishedScanners();
    void recomputeBatchStats(uint64_t batchId, bool allowSourceDelete = false);
    void enforceHistoryBound();

    int indexOfTask(uint64_t taskId) const;
    int indexOfBatch(uint64_t batchId) const;
    static bool isTaskTerminal(TransferStatus st);
    static bool isBatchTerminal(const TransferBatch& b);

    uint64_t nextTaskId_ = 1;
    uint64_t nextBatchId_ = 1;
    int maxConcurrent_ = 3;
    int maxHistory_ = kMaxFinishedTransferHistory;
    bool paused_ = false;
    std::atomic<bool> shuttingDown_{false};
    int inFlightCount_ = 0;

    std::vector<TransferBatch> batches_;
    std::vector<TransferTask> tasks_;
    QHash<uint64_t, bool> batchPausedByUser_;
    QHash<uint64_t, bool> batchHadManualRemoval_;

    // Active per-task cancel/pause flags (accessed from main thread & worker cleanup)
    QHash<uint64_t, TaskControl> runningControls_;

    // Thread-safe dispatch queue for persistent-while-busy worker pool
    std::mutex workMutex_;
    std::condition_variable workCv_;
    std::deque<DispatchedJob> readyQueue_;
    std::vector<WorkerThreadHandle> workerThreads_;

    // Async directory scan threads
    std::vector<ScanWorkerHandle> scanWorkers_;

    mutable std::mutex capMutex_;
    QHash<QString, EndpointCapabilities> capCache_;
};

}  // namespace xsync
