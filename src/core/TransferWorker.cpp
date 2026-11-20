#include "core/TransferWorker.h"

#include "core/FileSystemBackend.h"
#include "core/ProtocolNegotiator.h"
#include "core/ShellUtils.h"
#include "core/SshClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryFile>
#include <QUuid>
#include <algorithm>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace xsync {

namespace {

// Throttles GUI progress callbacks to <= 10 Hz so high-speed transfers never flood Qt's event queue.
class ThrottledProgress {
public:
    explicit ThrottledProgress(const TransferCallbacks& cb) : cb_(cb) {
        totalTimer_.start();
        emitTimer_.start();
    }

    void resetClock() {
        totalTimer_.restart();
        emitTimer_.restart();
        lastBytes_ = 0;
        lastTimeMs_ = 0;
        smoothedSpeed_ = 0.0;
    }

    void update(uint64_t transferred, uint64_t total, bool force = false, double explicitSpeed = -1.0) {
        const qint64 elapsed = emitTimer_.elapsed();
        if (!force && elapsed < 100) return;
        emitTimer_.restart();

        const qint64 nowMs = totalTimer_.elapsed();
        double speed = explicitSpeed;
        if (speed < 0.0) {
            const qint64 dtMs = nowMs - lastTimeMs_;
            if (dtMs > 50 && transferred >= lastBytes_) {
                double inst = static_cast<double>(transferred - lastBytes_) * 1000.0 /
                              static_cast<double>(dtMs);
                smoothedSpeed_ = (smoothedSpeed_ <= 0.1) ? inst : (0.65 * smoothedSpeed_ + 0.35 * inst);
                lastBytes_ = transferred;
                lastTimeMs_ = nowMs;
            }
            speed = smoothedSpeed_;
        }

        int pct = 0;
        if (total > 0) {
            pct = static_cast<int>(std::min<uint64_t>(100, (transferred * 100) / total));
        } else if (force) {
            pct = 100;
        }
        if (cb_.onProgress) {
            cb_.onProgress(transferred, total, pct, speed);
        }
    }

private:
    const TransferCallbacks& cb_;
    QElapsedTimer totalTimer_;
    QElapsedTimer emitTimer_;
    uint64_t lastBytes_ = 0;
    qint64 lastTimeMs_ = 0;
    double smoothedSpeed_ = 0.0;
};

struct TempFileGuard {
    QString path;
    ~TempFileGuard() {
        if (!path.isEmpty()) {
            QFile::remove(path);
        }
    }
};

// Creates a 0600 one-time token file for xsync's built-in SSH_ASKPASS mode.
QString createAskpassTokenFile(const QString& secret) {
    if (secret.isEmpty()) return {};
    const QString tmpPath = QDir::tempPath() + QStringLiteral("/xsync_ap_%1.tmp")
                                                   .arg(QUuid::createUuid().toString(QUuid::Id128));
    QFile f(tmpPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
#ifndef _WIN32
    ::chmod(QFile::encodeName(tmpPath).constData(), 0600);
#endif
    f.write(secret.toUtf8());
    f.close();
    return tmpPath;
}

QString uniqueLocalCopyTarget(const QString& destDir, const QString& name) {
    QString cand = LocalFileSystem::joinPath(destDir, name);
    if (!QFileInfo::exists(cand)) return cand;

    QFileInfo fi(name);
    const QString base = fi.completeBaseName();
    const QString ext = fi.suffix().isEmpty() ? QString() : (QLatin1Char('.') + fi.suffix());
    for (int i = 1; i < 10000; ++i) {
        QString suffix = (i == 1) ? QStringLiteral(" (copy)") : QStringLiteral(" (copy %1)").arg(i);
        QString next = LocalFileSystem::joinPath(destDir, base + suffix + ext);
        if (!QFileInfo::exists(next)) return next;
    }
    return cand;
}

uint64_t computeLocalSize(const QString& path, const std::atomic<bool>& cancelFlag) {
    QFileInfo fi(path);
    if (!fi.exists()) return 0;
    if (!fi.isDir()) return static_cast<uint64_t>(std::max<qint64>(0, fi.size()));
    uint64_t sum = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (cancelFlag.load(std::memory_order_relaxed)) break;
        it.next();
        sum += static_cast<uint64_t>(std::max<qint64>(0, it.fileInfo().size()));
    }
    return sum;
}

struct RemoteFilePlan {
    QString relPath;
    QString fullRemotePath;
    uint64_t size = 0;
    bool isDir = false;
};

bool collectRemoteTree(SshClient& client,
                       const QString& remoteRoot,
                       const QString& relPrefix,
                       std::vector<RemoteFilePlan>& outItems,
                       uint64_t& totalBytes,
                       const std::atomic<bool>& cancelFlag,
                       QString& errorOut) {
    if (cancelFlag.load(std::memory_order_relaxed)) {
        errorOut = QStringLiteral("Canceled");
        return false;
    }
    std::vector<FileEntry> entries;
    if (!client.listDirectory(remoteRoot, entries, errorOut, &cancelFlag)) {
        return false;
    }
    for (const FileEntry& e : entries) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        const QString rel = relPrefix.isEmpty() ? e.name : (relPrefix + QLatin1Char('/') + e.name);
        if (e.isDir && !e.isSymlink) {
            outItems.push_back({rel, e.fullPath, 0, true});
            if (!collectRemoteTree(client, e.fullPath, rel, outItems, totalBytes, cancelFlag, errorOut)) {
                return false;
            }
        } else {
            outItems.push_back({rel, e.fullPath, e.size, false});
            totalBytes += e.size;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 1. Local <-> Local Copy
// ---------------------------------------------------------------------------
bool runLocalToLocal(TransferTask& task,
                     const std::atomic<bool>& cancelFlag,
                     const TransferCallbacks& cb,
                     ThrottledProgress& prog,
                     QString& errorOut) {
    const QFileInfo srcInfo(task.sourcePath);
    if (!srcInfo.exists()) {
        errorOut = QStringLiteral("Local source does not exist: %1").arg(task.sourcePath);
        return false;
    }

    QString targetPath = LocalFileSystem::joinPath(task.destDir, task.itemName);
    if (QDir::cleanPath(targetPath) == QDir::cleanPath(task.sourcePath)) {
        targetPath = uniqueLocalCopyTarget(task.destDir, task.itemName);
    }

    const uint64_t totalBytes = computeLocalSize(task.sourcePath, cancelFlag);
    task.totalBytes = totalBytes;

    if (!srcInfo.isDir()) {
        bool ok = LocalFileSystem::copyFileStreaming(
            task.sourcePath, targetPath,
            [&](uint64_t done, uint64_t tot) { prog.update(done, tot); },
            &cancelFlag, errorOut);
        if (ok) prog.update(totalBytes, totalBytes, true);
        return ok;
    }

    // Recursive directory copy
    if (!LocalFileSystem::mkdir(targetPath, errorOut)) return false;
    const QDir baseDir(task.sourcePath);
    uint64_t completedBefore = 0;

    QDirIterator it(task.sourcePath,
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString rel = baseDir.relativeFilePath(fi.absoluteFilePath());
        const QString dst = LocalFileSystem::joinPath(targetPath, rel);

        if (fi.isDir() && !fi.isSymLink()) {
            if (!LocalFileSystem::mkdir(dst, errorOut)) return false;
        } else {
            const uint64_t fsize = static_cast<uint64_t>(std::max<qint64>(0, fi.size()));
            bool ok = LocalFileSystem::copyFileStreaming(
                fi.absoluteFilePath(), dst,
                [&](uint64_t done, uint64_t /*tot*/) {
                    prog.update(completedBefore + done, totalBytes);
                },
                &cancelFlag, errorOut);
            if (!ok) return false;
            completedBefore += fsize;
        }
    }
    prog.update(totalBytes, totalBytes, true);
    return true;
}

// ---------------------------------------------------------------------------
// 2. Local <-> Remote via rsync subprocess
// ---------------------------------------------------------------------------
bool runLocalRemoteRsync(TransferTask& task,
                         const EndpointCapabilities& localCaps,
                         const std::atomic<bool>& cancelFlag,
                         const TransferCallbacks& cb,
                         ThrottledProgress& prog,
                         QString& errorOut) {
    const bool isUpload = task.sourceEndpoint.isLocal();
    const SessionProfile& remote = isUpload ? task.destEndpoint.session : task.sourceEndpoint.session;

    // Build ssh -e transport command
    QStringList sshParts;
    if (localCaps.rsyncFlavor == RsyncFlavor::Wsl) {
        sshParts << QStringLiteral("ssh");
    } else {
        QString sshExe = ProtocolNegotiator::findLocalSshExecutable();
        if (sshExe.isEmpty()) sshExe = QStringLiteral("ssh");
#ifdef _WIN32
        if (localCaps.rsyncFlavor == RsyncFlavor::Cygwin || localCaps.rsyncFlavor == RsyncFlavor::Msys2) {
            sshExe = ShellUtils::toRsyncLocalPath(sshExe, localCaps.rsyncFlavor);
        }
#endif
        sshParts << ShellUtils::shellQuotePosix(sshExe);
    }

    sshParts << QStringLiteral("-p") << QString::number(remote.port)
             << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=accept-new")
             << QStringLiteral("-o") << QStringLiteral("ConnectTimeout=12");

    if (remote.useJumpHost && !remote.jumpHost.trimmed().isEmpty()) {
        const QString jUser = remote.jumpUsername.isEmpty() ? remote.username : remote.jumpUsername;
        QString jSpec = QStringLiteral("%1@%2").arg(jUser, remote.jumpHost.trimmed());
        if (remote.jumpPort != 22) jSpec += QStringLiteral(":%1").arg(remote.jumpPort);
        sshParts << QStringLiteral("-J") << ShellUtils::shellQuotePosix(jSpec);
    }

    TempFileGuard askpassGuard;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    if (remote.authType == AuthType::PrivateKey && !remote.privateKeyPath.isEmpty()) {
        QString keyPath = ShellUtils::expandTilde(remote.privateKeyPath);
        keyPath = ShellUtils::toRsyncLocalPath(keyPath, localCaps.rsyncFlavor);
        sshParts << QStringLiteral("-i") << ShellUtils::shellQuotePosix(keyPath);
        if (remote.passphrase.isEmpty()) {
            sshParts << QStringLiteral("-o") << QStringLiteral("BatchMode=yes");
        } else if (localCaps.rsyncFlavor != RsyncFlavor::Wsl) {
            askpassGuard.path = createAskpassTokenFile(remote.passphrase);
        } else {
            errorOut = QStringLiteral("WSL rsync does not support encrypted key passphrase via askpass");
            return false;
        }
    } else if (remote.authType == AuthType::SshAgent) {
        sshParts << QStringLiteral("-o") << QStringLiteral("BatchMode=yes");
    } else if (remote.authType == AuthType::Password) {
        if (localCaps.rsyncFlavor == RsyncFlavor::Wsl) {
            errorOut = QStringLiteral("WSL rsync cannot use password askpass; falling back to SFTP");
            return false;
        }
        askpassGuard.path = createAskpassTokenFile(remote.password);
        if (askpassGuard.path.isEmpty()) {
            errorOut = QStringLiteral("Cannot initialize SSH_ASKPASS token; falling back to SFTP");
            return false;
        }
        sshParts << QStringLiteral("-o") << QStringLiteral("PreferredAuthentications=password,keyboard-interactive")
                 << QStringLiteral("-o") << QStringLiteral("NumberOfPasswordPrompts=1");
    }

    if (!askpassGuard.path.isEmpty()) {
        env.insert(QStringLiteral("SSH_ASKPASS"), QCoreApplication::applicationFilePath());
        env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force"));
        env.insert(QStringLiteral("DISPLAY"), QStringLiteral("xsync:0"));
        env.insert(QStringLiteral("XSYNC_ASKPASS_MODE"), QStringLiteral("1"));
        env.insert(QStringLiteral("XSYNC_ASKPASS_TOKEN_FILE"), askpassGuard.path);
    }

    QStringList rsyncArgs;
    if (localCaps.rsyncFlavor == RsyncFlavor::Wsl) {
        rsyncArgs << QStringLiteral("--") << QStringLiteral("rsync");
    }
    rsyncArgs << QStringLiteral("-az")
              << QStringLiteral("--partial")
              << QStringLiteral("--info=progress2")
              << QStringLiteral("-s")
              << QStringLiteral("-e") << sshParts.join(QLatin1Char(' '));

    const QString remoteUserHost = QStringLiteral("%1@%2").arg(remote.username, remote.host);
    if (isUpload) {
        QString localSrc = ShellUtils::toRsyncLocalPath(task.sourcePath, localCaps.rsyncFlavor);
        QString remoteDst = QStringLiteral("%1:%2/")
                                .arg(remoteUserHost, normalizeRemotePath(task.destDir));
        rsyncArgs << localSrc << remoteDst;
    } else {
        QString remoteSrc = QStringLiteral("%1:%2")
                                .arg(remoteUserHost, normalizeRemotePath(task.sourcePath));
        QDir().mkpath(task.destDir);
        QString localDst = ShellUtils::toRsyncLocalPath(task.destDir, localCaps.rsyncFlavor);
        if (!localDst.endsWith(QLatin1Char('/'))) localDst.append(QLatin1Char('/'));
        rsyncArgs << remoteSrc << localDst;
    }

    if (cb.onLog) {
        cb.onLog(QStringLiteral("[Task #%1] Launching rsync (%2)")
                     .arg(task.id)
                     .arg(localCaps.rsyncFlavor == RsyncFlavor::Wsl ? QStringLiteral("WSL") : QStringLiteral("native")));
    }

    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.setProgram(localCaps.rsyncPath);
    proc.setArguments(rsyncArgs);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start();

    if (!proc.waitForStarted(5000)) {
        errorOut = QStringLiteral("Failed to launch rsync process: %1").arg(proc.errorString());
        return false;
    }

    QString stderrAccum;
    uint64_t knownTotal = isUpload ? computeLocalSize(task.sourcePath, cancelFlag) : task.totalBytes;

    while (proc.state() != QProcess::NotRunning) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            proc.terminate();
            if (!proc.waitForFinished(1500)) {
                proc.kill();
                proc.waitForFinished(1000);
            }
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        proc.waitForReadyRead(100);
        const QByteArray outChunk = proc.readAllStandardOutput();
        if (!outChunk.isEmpty()) {
            const auto parsed = ShellUtils::parseRsyncProgress2(QString::fromUtf8(outChunk));
            if (parsed.has_value()) {
                uint64_t tot = knownTotal;
                if (tot == 0 && parsed->percent > 0) {
                    tot = (parsed->bytesTransferred * 100) / static_cast<uint64_t>(parsed->percent);
                }
                prog.update(parsed->bytesTransferred, std::max(tot, parsed->bytesTransferred),
                            false, parsed->speedBytesPerSec);
            }
        }
        const QByteArray errChunk = proc.readAllStandardError();
        if (!errChunk.isEmpty() && stderrAccum.size() < 8192) {
            stderrAccum.append(QString::fromUtf8(errChunk));
        }
    }

    // Drain remaining output
    const QByteArray tailOut = proc.readAllStandardOutput();
    if (!tailOut.isEmpty()) {
        const auto parsed = ShellUtils::parseRsyncProgress2(QString::fromUtf8(tailOut));
        if (parsed.has_value()) {
            prog.update(parsed->bytesTransferred, std::max(knownTotal, parsed->bytesTransferred),
                        true, parsed->speedBytesPerSec);
        }
    }
    const QByteArray tailErr = proc.readAllStandardError();
    if (!tailErr.isEmpty() && stderrAccum.size() < 8192) {
        stderrAccum.append(QString::fromUtf8(tailErr));
    }

    if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
        prog.update(knownTotal, knownTotal, true);
        return true;
    }

    errorOut = QStringLiteral("rsync exited with code %1: %2")
                   .arg(proc.exitCode())
                   .arg(stderrAccum.trimmed().left(400));
    return false;
}

// ---------------------------------------------------------------------------
// 3. Local <-> Remote via native libssh2 SFTP or SCP
// ---------------------------------------------------------------------------
bool runLocalRemoteNative(TransferTask& task,
                          SshClient& client,
                          bool useSftp,
                          const std::atomic<bool>& cancelFlag,
                          ThrottledProgress& prog,
                          QString& errorOut) {
    const bool isUpload = task.sourceEndpoint.isLocal();

    if (isUpload) {
        const QFileInfo srcInfo(task.sourcePath);
        if (!srcInfo.exists()) {
            errorOut = QStringLiteral("Local path not found: %1").arg(task.sourcePath);
            return false;
        }
        const QString remoteTarget = joinRemotePath(task.destDir, task.itemName);
        const uint64_t totalBytes = computeLocalSize(task.sourcePath, cancelFlag);
        task.totalBytes = totalBytes;

        if (!srcInfo.isDir()) {
            if (!client.mkdirRecursive(task.destDir, errorOut, &cancelFlag)) return false;
            auto onProg = [&](uint64_t done, uint64_t tot) { prog.update(done, tot); };
            bool ok = useSftp ? client.uploadFileSftp(task.sourcePath, remoteTarget, onProg, &cancelFlag, errorOut)
                              : client.uploadFileScp(task.sourcePath, remoteTarget, onProg, &cancelFlag, errorOut);
            if (ok) prog.update(totalBytes, totalBytes, true);
            return ok;
        }

        // Recursive directory upload
        if (!client.mkdirRecursive(remoteTarget, errorOut, &cancelFlag)) return false;
        const QDir baseDir(task.sourcePath);
        uint64_t completedBefore = 0;

        QDirIterator it(task.sourcePath,
                        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            if (cancelFlag.load(std::memory_order_relaxed)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            it.next();
            const QFileInfo fi = it.fileInfo();
            const QString rel = baseDir.relativeFilePath(fi.absoluteFilePath());
            const QString rPath = joinRemotePath(remoteTarget, rel);

            if (fi.isDir() && !fi.isSymLink()) {
                if (!client.mkdirRecursive(rPath, errorOut, &cancelFlag)) return false;
            } else {
                const uint64_t fsize = static_cast<uint64_t>(std::max<qint64>(0, fi.size()));
                auto onProg = [&](uint64_t done, uint64_t /*tot*/) {
                    prog.update(completedBefore + done, totalBytes);
                };
                bool ok = useSftp
                              ? client.uploadFileSftp(fi.absoluteFilePath(), rPath, onProg, &cancelFlag, errorOut)
                              : client.uploadFileScp(fi.absoluteFilePath(), rPath, onProg, &cancelFlag, errorOut);
                if (!ok) return false;
                completedBefore += fsize;
            }
        }
        prog.update(totalBytes, totalBytes, true);
        return true;
    }

    // Download (Remote -> Local)
    FileEntry st;
    bool isDir = task.isDir;
    QString stErr;
    if (client.statPath(task.sourcePath, st, stErr, &cancelFlag)) {
        isDir = st.isDir;
        if (!isDir) task.totalBytes = st.size;
    }

    const QString localTarget = LocalFileSystem::joinPath(task.destDir, task.itemName);
    if (!isDir) {
        auto onProg = [&](uint64_t done, uint64_t tot) { prog.update(done, tot); };
        bool ok = useSftp ? client.downloadFileSftp(task.sourcePath, localTarget, onProg, &cancelFlag, errorOut)
                          : client.downloadFileScp(task.sourcePath, localTarget, onProg, &cancelFlag, errorOut);
        if (ok) prog.update(task.totalBytes, task.totalBytes, true);
        return ok;
    }

    // Recursive directory download
    if (!LocalFileSystem::mkdir(localTarget, errorOut)) return false;
    std::vector<RemoteFilePlan> tree;
    uint64_t totalBytes = 0;
    if (!collectRemoteTree(client, task.sourcePath, {}, tree, totalBytes, cancelFlag, errorOut)) {
        return false;
    }
    task.totalBytes = totalBytes;
    uint64_t completedBefore = 0;

    for (const RemoteFilePlan& item : tree) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        const QString lPath = LocalFileSystem::joinPath(localTarget, item.relPath);
        if (item.isDir) {
            if (!LocalFileSystem::mkdir(lPath, errorOut)) return false;
        } else {
            auto onProg = [&](uint64_t done, uint64_t /*tot*/) {
                prog.update(completedBefore + done, totalBytes);
            };
            bool ok = useSftp
                          ? client.downloadFileSftp(item.fullRemotePath, lPath, onProg, &cancelFlag, errorOut)
                          : client.downloadFileScp(item.fullRemotePath, lPath, onProg, &cancelFlag, errorOut);
            if (!ok) return false;
            completedBefore += item.size;
        }
    }
    prog.update(totalBytes, totalBytes, true);
    return true;
}

// ---------------------------------------------------------------------------
// 4. Remote A -> Remote B (Direct rsync/scp executed on Remote A)
// ---------------------------------------------------------------------------
bool runRemoteDirect(TransferTask& task,
                     SshClient& srcClient,
                     bool useRsync,
                     const std::atomic<bool>& cancelFlag,
                     const TransferCallbacks& cb,
                     ThrottledProgress& prog,
                     QString& errorOut) {
    const SessionProfile& src = task.sourceEndpoint.session;
    const SessionProfile& dst = task.destEndpoint.session;

    const QString srcPathQ = ShellUtils::shellQuotePosix(normalizeRemotePath(task.sourcePath));
    const QString dstDirNorm = normalizeRemotePath(task.destDir);
    const QString dstTargetSpec = QStringLiteral("%1@%2:%3/")
                                      .arg(dst.username, dst.host, dstDirNorm);

    QString cmd;
    if (useRsync) {
        const QString sshOpt = QStringLiteral(
            "ssh -p %1 -o BatchMode=yes -o ConnectTimeout=6 -o StrictHostKeyChecking=accept-new")
                                   .arg(dst.port);
        cmd = QStringLiteral("rsync -az --partial --info=progress2 -s -e %1 %2 %3")
                  .arg(ShellUtils::shellQuotePosix(sshOpt),
                       srcPathQ,
                       ShellUtils::shellQuotePosix(dstTargetSpec));
    } else {
        cmd = QStringLiteral(
                  "scp -r -P %1 -o BatchMode=yes -o ConnectTimeout=6 -o StrictHostKeyChecking=accept-new %2 %3")
                  .arg(dst.port)
                  .arg(srcPathQ, ShellUtils::shellQuotePosix(dstTargetSpec));
    }

    if (cb.onLog) {
        cb.onLog(QStringLiteral("[Task #%1] Attempting Direct Remote→Remote (%2): %3 → %4")
                     .arg(task.id)
                     .arg(useRsync ? QStringLiteral("rsync") : QStringLiteral("scp"),
                          src.displayAddress(), dst.displayAddress()));
    }

    ExecResult r = srcClient.execCommand(
        cmd, &cancelFlag, /*timeoutMs=*/0,
        [&](const QString& chunk, bool /*isStderr*/) {
            if (useRsync) {
                const auto parsed = ShellUtils::parseRsyncProgress2(chunk);
                if (parsed.has_value()) {
                    uint64_t tot = task.totalBytes;
                    if (tot == 0 && parsed->percent > 0) {
                        tot = (parsed->bytesTransferred * 100) / static_cast<uint64_t>(parsed->percent);
                    }
                    prog.update(parsed->bytesTransferred, std::max(tot, parsed->bytesTransferred),
                                false, parsed->speedBytesPerSec);
                }
            }
        },
        src.forwardAgent);

    if (r.ok && r.exitCode == 0) {
        prog.update(task.totalBytes, task.totalBytes, true);
        return true;
    }

    errorOut = !r.stderrText.trimmed().isEmpty()
                   ? r.stderrText.trimmed().left(300)
                   : (!r.errorMessage.isEmpty() ? r.errorMessage
                                                : QStringLiteral("Direct transfer exited with code %1").arg(r.exitCode));
    return false;
}

// ---------------------------------------------------------------------------
// 5. Remote A -> Remote B (Zero-Disk Local Streaming Relay, fixed 256 KiB RAM)
// ---------------------------------------------------------------------------
bool runRemoteRelay(TransferTask& task,
                    SshClient& srcClient,
                    bool srcUseSftp,
                    SshClient& dstClient,
                    bool dstUseSftp,
                    const std::atomic<bool>& cancelFlag,
                    ThrottledProgress& prog,
                    QString& errorOut) {
    FileEntry st;
    bool isDir = task.isDir;
    QString stErr;
    if (srcClient.statPath(task.sourcePath, st, stErr, &cancelFlag)) {
        isDir = st.isDir;
        if (!isDir) task.totalBytes = st.size;
    }

    const QString dstTarget = joinRemotePath(task.destDir, task.itemName);
    if (!isDir) {
        if (!dstClient.mkdirRecursive(task.destDir, errorOut, &cancelFlag)) return false;
        bool ok = SshClient::relayFileStream(
            srcClient, task.sourcePath, srcUseSftp,
            dstClient, dstTarget, dstUseSftp,
            [&](uint64_t done, uint64_t tot) { prog.update(done, tot); },
            &cancelFlag, errorOut);
        if (ok) prog.update(task.totalBytes, task.totalBytes, true);
        return ok;
    }

    // Recursive directory streaming relay
    if (!dstClient.mkdirRecursive(dstTarget, errorOut, &cancelFlag)) return false;
    std::vector<RemoteFilePlan> tree;
    uint64_t totalBytes = 0;
    if (!collectRemoteTree(srcClient, task.sourcePath, {}, tree, totalBytes, cancelFlag, errorOut)) {
        return false;
    }
    task.totalBytes = totalBytes;
    uint64_t completedBefore = 0;

    for (const RemoteFilePlan& item : tree) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        const QString dPath = joinRemotePath(dstTarget, item.relPath);
        if (item.isDir) {
            if (!dstClient.mkdirRecursive(dPath, errorOut, &cancelFlag)) return false;
        } else {
            bool ok = SshClient::relayFileStream(
                srcClient, item.fullRemotePath, srcUseSftp,
                dstClient, dPath, dstUseSftp,
                [&](uint64_t done, uint64_t /*tot*/) {
                    prog.update(completedBefore + done, totalBytes);
                },
                &cancelFlag, errorOut);
            if (!ok) return false;
            completedBefore += item.size;
        }
    }
    prog.update(totalBytes, totalBytes, true);
    return true;
}

}  // namespace

SshClient* WorkerSessionCache::acquire(const EndpointSpec& ep,
                                       QString& errorOut,
                                       const std::atomic<bool>* cancelFlag,
                                       bool slotB) {
    QString& key = slotB ? keyB : keyA;
    std::unique_ptr<SshClient>& slot = slotB ? clientB : clientA;
    const QString wantedKey = ep.cacheKey();

    if (slot && key == wantedKey && slot->isConnected()) {
        return slot.get();
    }
    slot.reset();
    key.clear();

    auto fresh = std::make_unique<SshClient>();
    if (!fresh->connectToHost(ep.session, errorOut, cancelFlag)) {
        return nullptr;
    }
    key = wantedKey;
    slot = std::move(fresh);
    return slot.get();
}

void WorkerSessionCache::clear() {
    clientA.reset();
    keyA.clear();
    clientB.reset();
    keyB.clear();
}

bool TransferWorker::execute(TransferTask& task,
                             const std::atomic<bool>& cancelFlag,
                             const TransferCallbacks& cb,
                             QString& errorOut,
                             WorkerSessionCache* sessionCache) {
    ThrottledProgress prog(cb);

    auto emitState = [&](TransferStatus st, TransferProtocol proto, const QString& msg) {
        task.status = st;
        task.activeProtocol = proto;
        task.statusMessage = msg;
        if (cb.onState) cb.onState(st, proto, msg);
    };

    if (cancelFlag.load(std::memory_order_relaxed)) {
        errorOut = QStringLiteral("Canceled");
        return false;
    }

    // -----------------------------------------------------------------------
    // Case A: Local <-> Local
    // -----------------------------------------------------------------------
    if (task.sourceEndpoint.isLocal() && task.destEndpoint.isLocal()) {
        emitState(TransferStatus::Running, TransferProtocol::LocalCopy, QStringLiteral("Copying locally..."));
        bool ok = runLocalToLocal(task, cancelFlag, cb, prog, errorOut);
        if (ok && task.deleteSourceAfter &&
            QDir::cleanPath(LocalFileSystem::joinPath(task.destDir, task.itemName)) !=
                QDir::cleanPath(task.sourcePath)) {
            QString delErr;
            LocalFileSystem::removePath(task.sourcePath, delErr);
        }
        return ok;
    }

    // -----------------------------------------------------------------------
    // Case B: Local <-> Remote
    // -----------------------------------------------------------------------
    if (task.sourceEndpoint.isLocal() != task.destEndpoint.isLocal()) {
        emitState(TransferStatus::Negotiating, TransferProtocol::Sftp, QStringLiteral("Connecting & probing..."));

        const bool isUpload = task.sourceEndpoint.isLocal();
        const EndpointSpec& remoteEp = isUpload ? task.destEndpoint : task.sourceEndpoint;

        std::unique_ptr<SshClient> ownedClient;
        SshClient* remoteClientPtr = nullptr;
        if (sessionCache) {
            remoteClientPtr = sessionCache->acquire(remoteEp, errorOut, &cancelFlag, false);
        } else {
            ownedClient = std::make_unique<SshClient>();
            if (ownedClient->connectToHost(remoteEp.session, errorOut, &cancelFlag)) {
                remoteClientPtr = ownedClient.get();
            }
        }
        if (!remoteClientPtr) {
            return false;
        }
        SshClient& remoteClient = *remoteClientPtr;

        EndpointCapabilities localCaps = cb.getOrProbeCaps
                                             ? cb.getOrProbeCaps(EndpointSpec::local(), nullptr)
                                             : ProtocolNegotiator::probeLocal();
        EndpointCapabilities remoteCaps = cb.getOrProbeCaps
                                              ? cb.getOrProbeCaps(remoteEp, &remoteClient)
                                              : ProtocolNegotiator::probeRemote(remoteClient, &cancelFlag);

        const EndpointCapabilities& srcCaps = isUpload ? localCaps : remoteCaps;
        const EndpointCapabilities& dstCaps = isUpload ? remoteCaps : localCaps;

        const NegotiationPlan plan = ProtocolNegotiator::negotiate(
            task.sourceEndpoint, task.destEndpoint, srcCaps, dstCaps,
            task.protocolPreference, task.r2rPolicy);

        if (cb.onLog) {
            cb.onLog(QStringLiteral("[Task #%1] Protocol negotiation: %2 → %3")
                         .arg(task.id)
                         .arg(protocolToString(plan.primary), plan.reason));
        }

        std::vector<TransferProtocol> attempts;
        attempts.push_back(plan.primary);
        for (TransferProtocol fb : plan.fallbacks) attempts.push_back(fb);

        QString lastErr;
        for (size_t i = 0; i < attempts.size(); ++i) {
            if (cancelFlag.load(std::memory_order_relaxed)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            const TransferProtocol proto = attempts[i];
            prog.resetClock();
            emitState(TransferStatus::Running, proto,
                      QStringLiteral("Transferring via %1...").arg(protocolToString(proto)));

            bool ok = false;
            QString attemptErr;
            if (proto == TransferProtocol::Rsync) {
                ok = runLocalRemoteRsync(task, localCaps, cancelFlag, cb, prog, attemptErr);
            } else if (proto == TransferProtocol::Sftp) {
                ok = runLocalRemoteNative(task, remoteClient, true, cancelFlag, prog, attemptErr);
            } else if (proto == TransferProtocol::Scp) {
                ok = runLocalRemoteNative(task, remoteClient, false, cancelFlag, prog, attemptErr);
            }

            if (ok) {
                if (task.deleteSourceAfter) {
                    QString delErr;
                    if (isUpload) {
                        LocalFileSystem::removePath(task.sourcePath, delErr);
                    } else if (task.isDir) {
                        remoteClient.removeDirRecursive(task.sourcePath, delErr, &cancelFlag);
                    } else {
                        remoteClient.removeFile(task.sourcePath, delErr, &cancelFlag);
                    }
                }
                return true;
            }

            lastErr = attemptErr;
            if (cancelFlag.load(std::memory_order_relaxed)) {
                errorOut = QStringLiteral("Canceled");
                return false;
            }
            // If cached connection dropped mid-transfer, drop it so next retry reconnects cleanly
            if (sessionCache && !remoteClient.isConnected()) {
                sessionCache->clear();
            }
            if (i + 1 < attempts.size() && cb.onLog) {
                cb.onLog(QStringLiteral("[Task #%1] %2 failed (%3); degrading to %4")
                             .arg(task.id)
                             .arg(protocolToString(proto), attemptErr,
                                  protocolToString(attempts[i + 1])));
            }
        }

        errorOut = lastErr.isEmpty() ? QStringLiteral("Transfer failed") : lastErr;
        return false;
    }

    // -----------------------------------------------------------------------
    // Case C: Remote A <-> Remote B
    // -----------------------------------------------------------------------
    emitState(TransferStatus::Negotiating, TransferProtocol::RemoteDirectRsync,
              QStringLiteral("Connecting to both remotes..."));

    std::unique_ptr<SshClient> ownedSrc;
    std::unique_ptr<SshClient> ownedDst;
    SshClient* srcPtr = nullptr;
    SshClient* dstPtr = nullptr;

    if (sessionCache) {
        srcPtr = sessionCache->acquire(task.sourceEndpoint, errorOut, &cancelFlag, false);
    } else {
        ownedSrc = std::make_unique<SshClient>();
        if (ownedSrc->connectToHost(task.sourceEndpoint.session, errorOut, &cancelFlag)) {
            srcPtr = ownedSrc.get();
        }
    }
    if (!srcPtr) {
        errorOut = QStringLiteral("Source remote failed: %1").arg(errorOut);
        return false;
    }

    if (sessionCache) {
        dstPtr = sessionCache->acquire(task.destEndpoint, errorOut, &cancelFlag, true);
    } else {
        ownedDst = std::make_unique<SshClient>();
        if (ownedDst->connectToHost(task.destEndpoint.session, errorOut, &cancelFlag)) {
            dstPtr = ownedDst.get();
        }
    }
    if (!dstPtr) {
        errorOut = QStringLiteral("Destination remote failed: %1").arg(errorOut);
        return false;
    }

    SshClient& srcClient = *srcPtr;
    SshClient& dstClient = *dstPtr;

    EndpointCapabilities srcCaps = cb.getOrProbeCaps
                                       ? cb.getOrProbeCaps(task.sourceEndpoint, &srcClient)
                                       : ProtocolNegotiator::probeRemote(srcClient, &cancelFlag);
    EndpointCapabilities dstCaps = cb.getOrProbeCaps
                                       ? cb.getOrProbeCaps(task.destEndpoint, &dstClient)
                                       : ProtocolNegotiator::probeRemote(dstClient, &cancelFlag);

    const NegotiationPlan plan = ProtocolNegotiator::negotiate(
        task.sourceEndpoint, task.destEndpoint, srcCaps, dstCaps,
        task.protocolPreference, task.r2rPolicy);

    if (cb.onLog) {
        cb.onLog(QStringLiteral("[Task #%1] Remote→Remote plan: %2 → %3")
                     .arg(task.id)
                     .arg(protocolToString(plan.primary), plan.reason));
    }

    std::vector<TransferProtocol> attempts;
    attempts.push_back(plan.primary);
    for (TransferProtocol fb : plan.fallbacks) attempts.push_back(fb);

    QString lastErr;
    for (size_t i = 0; i < attempts.size(); ++i) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        const TransferProtocol proto = attempts[i];
        prog.resetClock();
        emitState(TransferStatus::Running, proto,
                  QStringLiteral("Transferring via %1...").arg(protocolToString(proto)));

        bool ok = false;
        QString attemptErr;
        if (proto == TransferProtocol::RemoteDirectRsync) {
            ok = runRemoteDirect(task, srcClient, true, cancelFlag, cb, prog, attemptErr);
        } else if (proto == TransferProtocol::RemoteDirectScp) {
            ok = runRemoteDirect(task, srcClient, false, cancelFlag, cb, prog, attemptErr);
        } else if (proto == TransferProtocol::RemoteRelaySftp) {
            ok = runRemoteRelay(task, srcClient, srcCaps.hasSftp,
                                dstClient, dstCaps.hasSftp, cancelFlag, prog, attemptErr);
        } else if (proto == TransferProtocol::RemoteRelayScp) {
            ok = runRemoteRelay(task, srcClient, false, dstClient, false, cancelFlag, prog, attemptErr);
        }

        if (ok) {
            if (task.deleteSourceAfter) {
                QString delErr;
                if (task.isDir) srcClient.removeDirRecursive(task.sourcePath, delErr, &cancelFlag);
                else srcClient.removeFile(task.sourcePath, delErr, &cancelFlag);
            }
            return true;
        }

        lastErr = attemptErr;
        if (cancelFlag.load(std::memory_order_relaxed)) {
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        if (proto == TransferProtocol::RemoteDirectRsync) {
            const bool sshLayerFailed =
                attemptErr.contains(QLatin1String("Permission denied"), Qt::CaseInsensitive) ||
                attemptErr.contains(QLatin1String("Connection refused"), Qt::CaseInsensitive) ||
                attemptErr.contains(QLatin1String("Connection timed out"), Qt::CaseInsensitive) ||
                attemptErr.contains(QLatin1String("No route to host"), Qt::CaseInsensitive) ||
                attemptErr.contains(QLatin1String("Could not resolve"), Qt::CaseInsensitive);
            if (sshLayerFailed && i + 1 < attempts.size() &&
                attempts[i + 1] == TransferProtocol::RemoteDirectScp) {
                ++i;  // Jump directly to local streaming relay instead of retrying the same blocked SSH hop
            }
        }

        if (i + 1 < attempts.size() && cb.onLog) {
            cb.onLog(QStringLiteral("[Task #%1] %2 failed (%3); degrading to %4")
                         .arg(task.id)
                         .arg(protocolToString(proto), attemptErr,
                              protocolToString(attempts[i + 1])));
        }
    }

    errorOut = lastErr.isEmpty() ? QStringLiteral("Remote-to-Remote transfer failed") : lastErr;
    return false;
}

}  // namespace xsync
