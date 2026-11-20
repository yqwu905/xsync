#pragma once

#include "core/Types.h"

#include <QObject>
#include <atomic>
#include <functional>
#include <memory>

namespace xsync {

class SshClient;

// Per-worker-thread reusable SSH connection cache.
// Keeps SFTP/SCP/Relay connections warm across consecutive files in a folder
// transfer, then disconnects automatically via RAII when the worker goes idle.
struct WorkerSessionCache {
    QString keyA;
    std::unique_ptr<SshClient> clientA;
    QString keyB;
    std::unique_ptr<SshClient> clientB;

    SshClient* acquire(const EndpointSpec& ep,
                       QString& errorOut,
                       const std::atomic<bool>* cancelFlag,
                       bool slotB = false);
    void clear();
};

struct TransferCallbacks {
    std::function<void(TransferStatus status, TransferProtocol activeProto, const QString& msg)> onState;
    std::function<void(uint64_t transferred, uint64_t total, int percent, double speedBps)> onProgress;
    std::function<void(const QString& logLine)> onLog;
    std::function<EndpointCapabilities(const EndpointSpec& ep, SshClient* connectedClient)> getOrProbeCaps;
};

// Executes a single TransferTask synchronously on a worker thread.
// Strictly honors cancelFlag, performs protocol negotiation + automatic fallback,
// and keeps memory footprint strictly bounded (256 KiB streaming buffer).
class TransferWorker {
public:
    static bool execute(TransferTask& task,
                        const std::atomic<bool>& cancelFlag,
                        const TransferCallbacks& cb,
                        QString& errorOut,
                        WorkerSessionCache* sessionCache = nullptr);
};

}  // namespace xsync
