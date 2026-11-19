#pragma once

#include "core/Types.h"

#include <QHash>
#include <QObject>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace xsync {

class SshClient;

// Thread-safe, bounded-concurrency transfer manager with bounded history eviction.
// Prevents unbounded memory growth even across tens of thousands of completed transfers.
class TransferManager : public QObject {
    Q_OBJECT
public:
    explicit TransferManager(QObject* parent = nullptr);
    ~TransferManager() override;

    uint64_t enqueueTask(TransferTask task);
    void cancelTask(uint64_t taskId);
    void cancelAll();
    void retryTask(uint64_t taskId);
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

    const std::vector<TransferTask>& tasks() const { return tasks_; }
    int activeCount() const;
    int queuedCount() const;
    double aggregateSpeedBytesPerSec() const;

    void shutdown();

signals:
    void taskAdded(const xsync::TransferTask& task);
    void taskUpdated(const xsync::TransferTask& task);
    void taskRemoved(uint64_t taskId);
    void queueStatsChanged();
    void logMessage(const QString& message);
    void paneRefreshRequested(const QString& paneId);

private:
    void scheduleNext();
    void startTask(uint64_t taskId);
    void enforceHistoryBound();
    int indexOfTask(uint64_t taskId) const;
    void cleanupFinishedThreads();

    struct RunningHandle {
        uint64_t taskId = 0;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> doneFlag;
    };

    uint64_t nextTaskId_ = 1;
    int maxConcurrent_ = 3;
    int maxHistory_ = kMaxFinishedTransferHistory;
    bool paused_ = false;
    bool shuttingDown_ = false;

    std::vector<TransferTask> tasks_;
    std::vector<RunningHandle> activeWorkers_;

    mutable std::mutex capMutex_;
    QHash<QString, EndpointCapabilities> capCache_;
};

}  // namespace xsync
