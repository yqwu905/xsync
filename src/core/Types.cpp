#include "core/Types.h"

#include <QDir>
#include <cmath>

namespace xsync {

QString EndpointCapabilities::summaryBadge() const {
    if (!probed) {
        return QStringLiteral("Probing...");
    }
    QStringList parts;
    if (hasRsync) {
        parts << (rsyncFlavor == RsyncFlavor::Wsl ? QStringLiteral("rsync(WSL)") : QStringLiteral("rsync"));
    }
    if (hasSftp) parts << QStringLiteral("SFTP");
    if (hasScp) parts << QStringLiteral("SCP");
    if (parts.isEmpty()) return QStringLiteral("Basic");
    return parts.join(QStringLiteral(" | "));
}

QString formatBytes(uint64_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double val = static_cast<double>(bytes);
    int unit = 0;
    while (val >= 1024.0 && unit < 5) {
        val /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    return QStringLiteral("%1 %2").arg(val, 0, 'f', val >= 100.0 ? 1 : 2).arg(kUnits[unit]);
}

QString formatSpeed(double bytesPerSec) {
    if (bytesPerSec <= 0.5) return QStringLiteral("--");
    return formatBytes(static_cast<uint64_t>(bytesPerSec)) + QStringLiteral("/s");
}

QString formatEta(uint64_t remainingBytes, double bytesPerSec) {
    if (bytesPerSec <= 1.0 || remainingBytes == 0) return QStringLiteral("--");
    const uint64_t secs = static_cast<uint64_t>(std::ceil(static_cast<double>(remainingBytes) / bytesPerSec));
    if (secs < 60) {
        return QStringLiteral("%1s").arg(secs);
    }
    const uint64_t mins = secs / 60;
    const uint64_t remS = secs % 60;
    if (mins < 60) {
        return QStringLiteral("%1m %2s").arg(mins).arg(remS, 2, 10, QLatin1Char('0'));
    }
    const uint64_t hrs = mins / 60;
    const uint64_t remM = mins % 60;
    return QStringLiteral("%1h %2m").arg(hrs).arg(remM, 2, 10, QLatin1Char('0'));
}

QString formatPermissions(uint32_t mode, bool isDir, bool isSymlink) {
    QString s(10, QLatin1Char('-'));
    if (isSymlink) s[0] = QLatin1Char('l');
    else if (isDir) s[0] = QLatin1Char('d');

    const uint32_t bits[9] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
    const char chars[9] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
    for (int i = 0; i < 9; ++i) {
        if (mode & bits[i]) {
            s[i + 1] = QLatin1Char(chars[i]);
        }
    }
    return s;
}

QString protocolToString(TransferProtocol p) {
    switch (p) {
        case TransferProtocol::LocalCopy: return QStringLiteral("Local");
        case TransferProtocol::Rsync: return QStringLiteral("rsync");
        case TransferProtocol::Sftp: return QStringLiteral("SFTP");
        case TransferProtocol::Scp: return QStringLiteral("SCP");
        case TransferProtocol::RemoteDirectRsync: return QStringLiteral("rsync (Direct)");
        case TransferProtocol::RemoteDirectScp: return QStringLiteral("SCP (Direct)");
        case TransferProtocol::RemoteRelaySftp: return QStringLiteral("SFTP (Relay)");
        case TransferProtocol::RemoteRelayScp: return QStringLiteral("SCP (Relay)");
    }
    return QStringLiteral("Unknown");
}

QString protocolPreferenceToString(ProtocolPreference p) {
    switch (p) {
        case ProtocolPreference::Auto: return QStringLiteral("Auto (rsync → SFTP → SCP)");
        case ProtocolPreference::Rsync: return QStringLiteral("Prefer rsync");
        case ProtocolPreference::Sftp: return QStringLiteral("Force SFTP");
        case ProtocolPreference::Scp: return QStringLiteral("Force SCP");
    }
    return QStringLiteral("Auto");
}

QString r2rPolicyToString(RemoteToRemotePolicy p) {
    switch (p) {
        case RemoteToRemotePolicy::DirectThenRelay: return QStringLiteral("Auto (Direct → Relay)");
        case RemoteToRemotePolicy::RelayOnly: return QStringLiteral("Local Relay Only");
        case RemoteToRemotePolicy::DirectOnly: return QStringLiteral("Direct Only");
    }
    return QStringLiteral("Auto");
}

QString authTypeToString(AuthType a) {
    switch (a) {
        case AuthType::Password: return QStringLiteral("Password");
        case AuthType::PrivateKey: return QStringLiteral("Private Key");
        case AuthType::SshAgent: return QStringLiteral("SSH Agent");
    }
    return QStringLiteral("Password");
}

QString transferStatusToString(TransferStatus s) {
    switch (s) {
        case TransferStatus::Queued: return QStringLiteral("Queued");
        case TransferStatus::Negotiating: return QStringLiteral("Negotiating");
        case TransferStatus::Running: return QStringLiteral("Transferring");
        case TransferStatus::Completed: return QStringLiteral("Completed");
        case TransferStatus::Failed: return QStringLiteral("Failed");
        case TransferStatus::Canceled: return QStringLiteral("Canceled");
    }
    return QStringLiteral("Unknown");
}

QString directionLabel(const EndpointSpec& src, const EndpointSpec& dst, TransferProtocol proto) {
    if (src.isLocal() && dst.isLocal()) return QStringLiteral("Local → Local");
    if (src.isLocal() && dst.isRemote()) return QStringLiteral("Upload (Local → Remote)");
    if (src.isRemote() && dst.isLocal()) return QStringLiteral("Download (Remote → Local)");
    if (proto == TransferProtocol::RemoteDirectRsync || proto == TransferProtocol::RemoteDirectScp) {
        return QStringLiteral("Remote → Remote (Direct)");
    }
    return QStringLiteral("Remote → Remote (Relay)");
}

QString normalizeRemotePath(const QString& path) {
    QString p = path.trimmed();
    if (p.isEmpty()) return QStringLiteral("/");
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!p.startsWith(QLatin1Char('/'))) {
        p.prepend(QLatin1Char('/'));
    }
    QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList resolved;
    for (const QString& part : parts) {
        if (part == QLatin1String(".")) continue;
        if (part == QLatin1String("..")) {
            if (!resolved.isEmpty()) resolved.removeLast();
        } else {
            resolved.append(part);
        }
    }
    return QLatin1Char('/') + resolved.join(QLatin1Char('/'));
}

QString joinRemotePath(const QString& baseDir, const QString& childName) {
    QString base = normalizeRemotePath(baseDir);
    QString child = childName;
    while (child.startsWith(QLatin1Char('/'))) child.remove(0, 1);
    while (child.endsWith(QLatin1Char('/')) && child.size() > 1) child.chop(1);
    if (child.isEmpty()) return base;
    if (base == QLatin1String("/")) return QLatin1Char('/') + child;
    return base + QLatin1Char('/') + child;
}

QString parentRemotePath(const QString& path) {
    QString norm = normalizeRemotePath(path);
    if (norm == QLatin1String("/")) return norm;
    int idx = norm.lastIndexOf(QLatin1Char('/'));
    if (idx <= 0) return QStringLiteral("/");
    return norm.left(idx);
}

QString baseNameAnyPath(const QString& path) {
    QString p = path;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (p.size() > 1 && p.endsWith(QLatin1Char('/'))) {
        p.chop(1);
    }
    int idx = p.lastIndexOf(QLatin1Char('/'));
    return (idx >= 0) ? p.mid(idx + 1) : p;
}

}  // namespace xsync
