#include "core/ProtocolNegotiator.h"

#include "core/SshClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <mutex>

namespace xsync {

namespace {

QString runQuickProbe(const QString& program, const QStringList& args, int timeoutMs = 2500) {
    QProcess proc;
    proc.setProgram(program);
    proc.setArguments(args);
    proc.start();
    if (!proc.waitForStarted(1500)) return {};
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(500);
        return {};
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        return {};
    }
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

#ifdef _WIN32
RsyncFlavor classifyWindowsRsync(const QString& exePath) {
    const QString lower = exePath.toLower();
    if (lower.contains(QLatin1String("msys")) || lower.contains(QLatin1String("git"))) {
        return RsyncFlavor::Msys2;
    }
    return RsyncFlavor::Cygwin;  // cwRsync / Cygwin default
}
#endif

}  // namespace

QString ProtocolNegotiator::findLocalSshExecutable() {
#ifdef _WIN32
    QString ssh = QStandardPaths::findExecutable(QStringLiteral("ssh.exe"));
    if (!ssh.isEmpty()) return ssh;
    const QStringList candidates = {
        QStringLiteral("C:/Windows/System32/OpenSSH/ssh.exe"),
        QStringLiteral("C:/Program Files/Git/usr/bin/ssh.exe"),
        QStringLiteral("C:/msys64/usr/bin/ssh.exe"),
    };
    for (const QString& c : candidates) {
        if (QFileInfo::exists(c)) return c;
    }
    return {};
#else
    return QStandardPaths::findExecutable(QStringLiteral("ssh"));
#endif
}

EndpointCapabilities ProtocolNegotiator::probeLocal(bool forceRefresh) {
    static std::mutex m;
    static EndpointCapabilities cached;

    std::lock_guard<std::mutex> lock(m);
    if (cached.probed && !forceRefresh) {
        return cached;
    }

    EndpointCapabilities caps;
    caps.probed = true;
    caps.hasSftp = true;  // Native libssh2 built-in
    caps.hasScp = true;   // Native libssh2 built-in
    caps.hasSsh = !findLocalSshExecutable().isEmpty();

#ifdef _WIN32
    caps.osHint = QStringLiteral("Windows");

    // 1. Check bundled bin/rsync.exe beside xsync.exe
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/bin/rsync.exe"),
        appDir + QStringLiteral("/rsync.exe"),
        QStandardPaths::findExecutable(QStringLiteral("rsync.exe")),
        QStringLiteral("C:/cwRsync/bin/rsync.exe"),
        QStringLiteral("C:/msys64/usr/bin/rsync.exe"),
        QStringLiteral("C:/Program Files/Git/usr/bin/rsync.exe"),
    };

    for (const QString& cand : candidates) {
        if (cand.isEmpty() || !QFileInfo::exists(cand)) continue;
        QString ver = runQuickProbe(cand, {QStringLiteral("--version")});
        if (!ver.isEmpty()) {
            caps.hasRsync = true;
            caps.rsyncPath = cand;
            caps.rsyncFlavor = classifyWindowsRsync(cand);
            caps.rsyncVersion = ver.section(QLatin1Char('\n'), 0, 0).trimmed();
            break;
        }
    }

    // 2. Fallback: check WSL rsync if no native Windows rsync found
    if (!caps.hasRsync) {
        const QString wsl = QStandardPaths::findExecutable(QStringLiteral("wsl.exe"));
        if (!wsl.isEmpty()) {
            QString ver = runQuickProbe(wsl, {QStringLiteral("--"), QStringLiteral("rsync"), QStringLiteral("--version")}, 3000);
            if (!ver.isEmpty() && ver.contains(QLatin1String("rsync"), Qt::CaseInsensitive)) {
                caps.hasRsync = true;
                caps.rsyncPath = wsl;
                caps.rsyncFlavor = RsyncFlavor::Wsl;
                caps.rsyncVersion = ver.section(QLatin1Char('\n'), 0, 0).trimmed();
            }
        }
    }
#else
#ifdef __APPLE__
    caps.osHint = QStringLiteral("macOS");
#else
    caps.osHint = QStringLiteral("Linux");
#endif
    const QString rsyncExe = QStandardPaths::findExecutable(QStringLiteral("rsync"));
    if (!rsyncExe.isEmpty()) {
        QString ver = runQuickProbe(rsyncExe, {QStringLiteral("--version")});
        if (!ver.isEmpty()) {
            caps.hasRsync = true;
            caps.rsyncPath = rsyncExe;
            caps.rsyncFlavor = RsyncFlavor::NativePosix;
            caps.rsyncVersion = ver.section(QLatin1Char('\n'), 0, 0).trimmed();
        }
    }
#endif

    cached = caps;
    return cached;
}

EndpointCapabilities ProtocolNegotiator::probeRemote(SshClient& client,
                                                     const std::atomic<bool>* cancelFlag) {
    EndpointCapabilities caps;
    caps.probed = true;

    QString sftpErr;
    caps.hasSftp = client.ensureSftp(sftpErr, cancelFlag);

    // Single combined shell probe to minimize round-trips
    const QString probeCmd = QStringLiteral(
        "echo \"OS=$(uname -s 2>/dev/null || echo Unknown)\";"
        "RS=$(command -v rsync 2>/dev/null || true);"
        "echo \"RSYNC=$RS\";"
        "if [ -n \"$RS\" ]; then echo \"RSYNC_VER=$($RS --version 2>/dev/null | head -n 1)\"; fi;"
        "echo \"SCP=$(command -v scp 2>/dev/null || true)\";"
        "echo \"SSH=$(command -v ssh 2>/dev/null || true)\"");

    ExecResult r = client.execCommand(probeCmd, cancelFlag, 5000);
    if (r.ok) {
        const QStringList lines = r.stdoutText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith(QLatin1String("OS="))) {
                caps.osHint = trimmed.mid(3).trimmed();
            } else if (trimmed.startsWith(QLatin1String("RSYNC="))) {
                caps.rsyncPath = trimmed.mid(6).trimmed();
                caps.hasRsync = !caps.rsyncPath.isEmpty();
                if (caps.hasRsync) caps.rsyncFlavor = RsyncFlavor::NativePosix;
            } else if (trimmed.startsWith(QLatin1String("RSYNC_VER="))) {
                caps.rsyncVersion = trimmed.mid(10).trimmed();
            } else if (trimmed.startsWith(QLatin1String("SCP="))) {
                caps.hasScp = !trimmed.mid(4).trimmed().isEmpty();
            } else if (trimmed.startsWith(QLatin1String("SSH="))) {
                caps.hasSsh = !trimmed.mid(4).trimmed().isEmpty();
            }
        }
    }

    // Even if `command -v scp` failed in non-standard PATH, libssh2 native SCP may still work
    if (!caps.hasSftp && !caps.hasScp) {
        caps.hasScp = true;
    }
    return caps;
}

NegotiationPlan ProtocolNegotiator::negotiate(const EndpointSpec& src,
                                              const EndpointSpec& dst,
                                              const EndpointCapabilities& srcCaps,
                                              const EndpointCapabilities& dstCaps,
                                              ProtocolPreference pref,
                                              RemoteToRemotePolicy r2rPolicy) {
    NegotiationPlan plan;

    // Case 1: Local <-> Local
    if (src.isLocal() && dst.isLocal()) {
        plan.primary = TransferProtocol::LocalCopy;
        plan.reason = QStringLiteral("Local filesystem streaming copy");
        return plan;
    }

    // Case 2: Local <-> Remote (Upload or Download)
    if (src.isLocal() != dst.isLocal()) {
        const EndpointCapabilities& localCaps = src.isLocal() ? srcCaps : dstCaps;
        const EndpointCapabilities& remoteCaps = src.isRemote() ? srcCaps : dstCaps;
        const bool bothHaveRsync = localCaps.hasRsync && remoteCaps.hasRsync &&
                                   (localCaps.hasSsh || localCaps.rsyncFlavor == RsyncFlavor::Wsl);

        if (pref == ProtocolPreference::Sftp) {
            if (remoteCaps.hasSftp) {
                plan.primary = TransferProtocol::Sftp;
                plan.fallbacks = {TransferProtocol::Scp};
                plan.reason = QStringLiteral("User selected SFTP");
            } else {
                plan.primary = TransferProtocol::Scp;
                plan.reason = QStringLiteral("SFTP unavailable on remote; degraded to SCP");
            }
            return plan;
        }

        if (pref == ProtocolPreference::Scp) {
            plan.primary = TransferProtocol::Scp;
            if (remoteCaps.hasSftp) plan.fallbacks = {TransferProtocol::Sftp};
            plan.reason = QStringLiteral("User selected SCP");
            return plan;
        }

        // Auto or Prefer Rsync: rsync first if BOTH sides support it, then SFTP, then SCP
        if (bothHaveRsync) {
            plan.primary = TransferProtocol::Rsync;
            if (remoteCaps.hasSftp) plan.fallbacks.push_back(TransferProtocol::Sftp);
            plan.fallbacks.push_back(TransferProtocol::Scp);
            plan.reason = QStringLiteral("Both local and remote have rsync; selected rsync with SFTP/SCP fallback");
        } else if (remoteCaps.hasSftp) {
            plan.primary = TransferProtocol::Sftp;
            plan.fallbacks.push_back(TransferProtocol::Scp);
            if (!localCaps.hasRsync && !remoteCaps.hasRsync) {
                plan.reason = QStringLiteral("rsync unavailable on both sides; degraded to SFTP");
            } else if (!localCaps.hasRsync) {
                plan.reason = QStringLiteral("Local rsync unavailable; degraded to SFTP");
            } else if (!localCaps.hasSsh && localCaps.rsyncFlavor != RsyncFlavor::Wsl) {
                plan.reason = QStringLiteral("Local OpenSSH client unavailable for rsync; degraded to SFTP");
            } else {
                plan.reason = QStringLiteral("Remote rsync unavailable; degraded to SFTP");
            }
        } else {
            plan.primary = TransferProtocol::Scp;
            plan.reason = QStringLiteral("Neither rsync nor SFTP available; degraded to SCP");
        }
        return plan;
    }

    // Case 3: Remote A <-> Remote B
    const bool bothHaveRsync = srcCaps.hasRsync && dstCaps.hasRsync;
    const bool canAttemptDirect = (r2rPolicy != RemoteToRemotePolicy::RelayOnly) && srcCaps.hasSsh;
    const TransferProtocol bestRelay = (srcCaps.hasSftp && dstCaps.hasSftp)
                                           ? TransferProtocol::RemoteRelaySftp
                                           : TransferProtocol::RemoteRelayScp;

    auto addRelayFallbacks = [&]() {
        if (r2rPolicy == RemoteToRemotePolicy::DirectOnly) return;
        plan.fallbacks.push_back(bestRelay);
        if (bestRelay == TransferProtocol::RemoteRelaySftp) {
            plan.fallbacks.push_back(TransferProtocol::RemoteRelayScp);
        }
    };

    if (r2rPolicy == RemoteToRemotePolicy::RelayOnly || !canAttemptDirect) {
        if (pref == ProtocolPreference::Scp) {
            plan.primary = TransferProtocol::RemoteRelayScp;
            plan.reason = QStringLiteral("Remote-to-Remote relay (SCP selected)");
        } else {
            plan.primary = bestRelay;
            if (bestRelay == TransferProtocol::RemoteRelaySftp) {
                plan.fallbacks.push_back(TransferProtocol::RemoteRelayScp);
            }
            plan.reason = (r2rPolicy == RemoteToRemotePolicy::RelayOnly)
                              ? QStringLiteral("Remote-to-Remote local zero-disk streaming relay")
                              : QStringLiteral("Source host lacks ssh client for direct mode; using local streaming relay");
        }
        return plan;
    }

    // DirectThenRelay or DirectOnly
    if (pref == ProtocolPreference::Sftp) {
        if (r2rPolicy == RemoteToRemotePolicy::DirectOnly) {
            plan.primary = srcCaps.hasScp ? TransferProtocol::RemoteDirectScp : TransferProtocol::RemoteDirectRsync;
            plan.reason = QStringLiteral("Direct-only policy overrides SFTP with direct remote transfer");
        } else {
            plan.primary = bestRelay;
            plan.reason = QStringLiteral("User selected SFTP; using local zero-disk SFTP streaming relay");
        }
        return plan;
    }

    if (pref == ProtocolPreference::Scp) {
        plan.primary = TransferProtocol::RemoteDirectScp;
        if (r2rPolicy != RemoteToRemotePolicy::DirectOnly) {
            plan.fallbacks.push_back(TransferProtocol::RemoteRelayScp);
            if (srcCaps.hasSftp && dstCaps.hasSftp) {
                plan.fallbacks.push_back(TransferProtocol::RemoteRelaySftp);
            }
        }
        plan.reason = QStringLiteral("Direct Remote→Remote SCP with relay fallback");
        return plan;
    }

    // Auto or Prefer Rsync for Remote <-> Remote
    if (bothHaveRsync) {
        plan.primary = TransferProtocol::RemoteDirectRsync;
        if (srcCaps.hasScp) plan.fallbacks.push_back(TransferProtocol::RemoteDirectScp);
        addRelayFallbacks();
        plan.reason = (r2rPolicy == RemoteToRemotePolicy::DirectOnly)
                          ? QStringLiteral("Both remotes have rsync; using Direct Remote→Remote rsync")
                          : QStringLiteral("Both remotes have rsync; trying Direct rsync first, auto-fallback to local stream relay");
    } else if (srcCaps.hasScp) {
        plan.primary = TransferProtocol::RemoteDirectScp;
        addRelayFallbacks();
        plan.reason = QStringLiteral("rsync missing on at least one remote; trying Direct SCP first, auto-fallback to local stream relay");
    } else {
        plan.primary = bestRelay;
        plan.reason = QStringLiteral("Neither rsync nor scp available on source remote; using local streaming relay");
    }
    return plan;
}

}  // namespace xsync
