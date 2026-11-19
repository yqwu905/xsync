#pragma once

#include "core/Types.h"

#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct _LIBSSH2_SESSION;
struct _LIBSSH2_SFTP;
struct _LIBSSH2_CHANNEL;
struct _LIBSSH2_SFTP_HANDLE;

namespace xsync {

// Ensures libssh2_init() and WinSock WSAStartup() are initialized once per process.
class SshGlobalInit {
public:
    SshGlobalInit();
    ~SshGlobalInit();
    SshGlobalInit(const SshGlobalInit&) = delete;
    SshGlobalInit& operator=(const SshGlobalInit&) = delete;
};

struct ExecResult {
    bool ok = false;
    int exitCode = -1;
    QString stdoutText;
    QString stderrText;
    QString errorMessage;
};

using ProgressCallback = std::function<void(uint64_t transferredBytes, uint64_t totalBytes)>;
using OutputChunkCallback = std::function<void(const QString& chunk, bool isStderr)>;

// Thread-confined, strictly RAII SSH/SFTP/SCP client backed by libssh2 + OpenSSL.
// Never share a single SshClient instance concurrently across threads; each pane or
// transfer worker owns its own SshClient instance.
class SshClient {
public:
    SshClient();
    ~SshClient();

    SshClient(const SshClient&) = delete;
    SshClient& operator=(const SshClient&) = delete;

    bool connectToHost(const SessionProfile& profile,
                       QString& errorOut,
                       const std::atomic<bool>* cancelFlag = nullptr,
                       int timeoutMs = 15000);
    void disconnect();

    bool isConnected() const;
    bool ensureSftp(QString& errorOut, const std::atomic<bool>* cancelFlag = nullptr);
    bool hasSftp() const { return sftp_ != nullptr; }

    // Keepalive ping (returns false if connection died)
    bool sendKeepAlive();

    // Execute remote shell command with optional live stdout/stderr streaming
    ExecResult execCommand(const QString& command,
                           const std::atomic<bool>* cancelFlag = nullptr,
                           int timeoutMs = 15000,
                           const OutputChunkCallback& onChunk = nullptr,
                           bool requestAgentForward = false);

    // Directory & metadata operations (uses SFTP if available, falls back to exec for minimal servers)
    QString resolveHomeDirectory(QString& errorOut, const std::atomic<bool>* cancelFlag = nullptr);
    bool listDirectory(const QString& remotePath,
                       std::vector<FileEntry>& outEntries,
                       QString& errorOut,
                       const std::atomic<bool>* cancelFlag = nullptr);
    bool statPath(const QString& remotePath,
                  FileEntry& outEntry,
                  QString& errorOut,
                  const std::atomic<bool>* cancelFlag = nullptr);
    bool mkdir(const QString& remotePath,
               uint32_t mode,
               QString& errorOut,
               const std::atomic<bool>* cancelFlag = nullptr);
    bool mkdirRecursive(const QString& remotePath,
                        QString& errorOut,
                        const std::atomic<bool>* cancelFlag = nullptr);
    bool renamePath(const QString& oldPath,
                    const QString& newPath,
                    QString& errorOut,
                    const std::atomic<bool>* cancelFlag = nullptr);
    bool removeFile(const QString& remotePath,
                    QString& errorOut,
                    const std::atomic<bool>* cancelFlag = nullptr);
    bool removeDirRecursive(const QString& remotePath,
                            QString& errorOut,
                            const std::atomic<bool>* cancelFlag = nullptr);
    bool chmodPath(const QString& remotePath,
                   uint32_t mode,
                   QString& errorOut,
                   const std::atomic<bool>* cancelFlag = nullptr);

    // Local <-> Remote streaming transfers using fixed 256 KiB buffer (zero whole-file buffering)
    bool uploadFileSftp(const QString& localPath,
                        const QString& remotePath,
                        const ProgressCallback& progressCb,
                        const std::atomic<bool>* cancelFlag,
                        QString& errorOut);
    bool downloadFileSftp(const QString& remotePath,
                          const QString& localPath,
                          const ProgressCallback& progressCb,
                          const std::atomic<bool>* cancelFlag,
                          QString& errorOut);

    bool uploadFileScp(const QString& localPath,
                       const QString& remotePath,
                       const ProgressCallback& progressCb,
                       const std::atomic<bool>* cancelFlag,
                       QString& errorOut);
    bool downloadFileScp(const QString& remotePath,
                         const QString& localPath,
                         const ProgressCallback& progressCb,
                         const std::atomic<bool>* cancelFlag,
                         QString& errorOut);

    // Remote A -> Remote B bounded-memory streaming relay (zero local disk temp files).
    // Reads chunks directly from `srcClient` and writes directly into `dstClient` using a
    // single reusable 256 KiB buffer. Supports SFTP->SFTP, SFTP->SCP, SCP->SFTP, SCP->SCP.
    static bool relayFileStream(SshClient& srcClient,
                                const QString& srcRemotePath,
                                bool srcUseSftp,
                                SshClient& dstClient,
                                const QString& dstRemotePath,
                                bool dstUseSftp,
                                const ProgressCallback& progressCb,
                                const std::atomic<bool>* cancelFlag,
                                QString& errorOut);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool connectRaw(const QString& host,
                    uint16_t port,
                    const QString& username,
                    AuthType authType,
                    const QString& password,
                    const QString& privateKeyPath,
                    const QString& passphrase,
                    int keepAliveSec,
                    QString& errorOut,
                    const std::atomic<bool>* cancelFlag,
                    int timeoutMs);

    bool waitSocket(int timeoutMs, const std::atomic<bool>* cancelFlag);
    QString lastSshError(const QString& fallback) const;
    QString lastSftpError(const QString& fallback) const;

    _LIBSSH2_SESSION* session_ = nullptr;
    _LIBSSH2_SFTP* sftp_ = nullptr;
    int64_t socketFd_ = -1;

    // Optional jump host tunnel resources (cleaned up in disconnect())
    std::unique_ptr<SshClient> jumpClient_;
    std::atomic<bool> tunnelStop_{false};
    std::thread tunnelThread_;
    int64_t listenFd_ = -1;
};

}  // namespace xsync
