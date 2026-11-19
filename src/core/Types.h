#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <cstdint>
#include <vector>

namespace xsync {

// Fixed streaming chunk size (256 KiB): large enough to saturate libssh2 SFTP
// pipelining while keeping per-transfer RAM strictly bounded and flat.
inline constexpr size_t kTransferBufferSize = 256 * 1024;
inline constexpr int kMaxFinishedTransferHistory = 500;
inline constexpr int kMaxLogLines = 1000;
inline constexpr const char* kXsyncMimeType = "application/x-xsync-entries";

enum class AuthType {
    Password = 0,
    PrivateKey = 1,
    SshAgent = 2
};

enum class ProtocolPreference {
    Auto = 0,  // rsync (if both sides have it) -> SFTP -> SCP
    Rsync = 1,
    Sftp = 2,
    Scp = 3
};

enum class TransferProtocol {
    LocalCopy = 0,
    Rsync = 1,
    Sftp = 2,
    Scp = 3,
    RemoteDirectRsync = 4,
    RemoteDirectScp = 5,
    RemoteRelaySftp = 6,
    RemoteRelayScp = 7
};

enum class RemoteToRemotePolicy {
    DirectThenRelay = 0,  // Prefer direct RemoteA -> RemoteB; auto-fallback to local stream relay
    RelayOnly = 1,
    DirectOnly = 2
};

enum class RsyncFlavor {
    None = 0,
    NativePosix,  // Linux / macOS / BSD
    Cygwin,       // cwRsync / Cygwin (/cygdrive/c/...)
    Msys2,        // MSYS2 / Git Bash (/c/...)
    Wsl           // Windows Subsystem for Linux (wsl.exe rsync, /mnt/c/...)
};

enum class EndpointType {
    Local = 0,
    Remote = 1
};

struct EndpointCapabilities {
    bool probed = false;
    bool hasRsync = false;
    RsyncFlavor rsyncFlavor = RsyncFlavor::None;
    QString rsyncPath;
    QString rsyncVersion;
    bool hasSftp = false;
    bool hasScp = false;
    bool hasSsh = false;
    QString osHint;

    QString summaryBadge() const;
};

struct SessionProfile {
    QString id;
    QString name;
    QString group;
    QString host;
    uint16_t port = 22;
    QString username;
    AuthType authType = AuthType::Password;
    QString password;        // Plaintext in memory only; encrypted by SecretStore on disk
    QString privateKeyPath;
    QString passphrase;      // Plaintext in memory only; encrypted by SecretStore on disk
    QString defaultRemotePath;
    QString defaultLocalPath;
    ProtocolPreference protocolPreference = ProtocolPreference::Auto;
    RemoteToRemotePolicy r2rPolicy = RemoteToRemotePolicy::DirectThenRelay;
    bool forwardAgent = true;
    int keepAliveSec = 30;

    // Optional JumpHost / Bastion
    bool useJumpHost = false;
    QString jumpHost;
    uint16_t jumpPort = 22;
    QString jumpUsername;
    AuthType jumpAuthType = AuthType::SshAgent;
    QString jumpPassword;
    QString jumpPrivateKeyPath;
    QString jumpPassphrase;

    bool isValid() const {
        return !host.trimmed().isEmpty() && !username.trimmed().isEmpty() && port > 0;
    }

    QString displayAddress() const {
        if (port == 22) {
            return QStringLiteral("%1@%2").arg(username, host);
        }
        return QStringLiteral("%1@%2:%3").arg(username, host).arg(port);
    }

    QString displayTitle() const {
        return name.trimmed().isEmpty() ? displayAddress() : name;
    }

    static SessionProfile createNew() {
        SessionProfile p;
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        p.group = QStringLiteral("Default");
        return p;
    }
};

struct EndpointSpec {
    EndpointType type = EndpointType::Local;
    SessionProfile session;  // Valid when type == EndpointType::Remote

    bool isLocal() const { return type == EndpointType::Local; }
    bool isRemote() const { return type == EndpointType::Remote; }

    QString label() const {
        return isLocal() ? QStringLiteral("Local") : session.displayTitle();
    }

    QString cacheKey() const {
        if (isLocal()) return QStringLiteral("local");
        return QStringLiteral("%1@%2:%3").arg(session.username, session.host).arg(session.port);
    }

    static EndpointSpec local() {
        EndpointSpec e;
        e.type = EndpointType::Local;
        return e;
    }

    static EndpointSpec remote(const SessionProfile& s) {
        EndpointSpec e;
        e.type = EndpointType::Remote;
        e.session = s;
        return e;
    }
};

struct FileEntry {
    QString name;
    QString fullPath;
    uint64_t size = 0;
    int64_t modifiedEpoch = 0;
    uint32_t permissions = 0;
    bool isDir = false;
    bool isSymlink = false;
    bool isParentLink = false;  // ".."
    QString symlinkTarget;
    QString owner;
    QString group;
};

enum class TransferStatus {
    Queued = 0,
    Negotiating = 1,
    Running = 2,
    Completed = 3,
    Failed = 4,
    Canceled = 5
};

struct TransferTask {
    uint64_t id = 0;
    EndpointSpec sourceEndpoint;
    EndpointSpec destEndpoint;
    QString sourcePath;       // Full source file or directory path
    QString destDir;          // Target directory on destination endpoint
    QString itemName;         // File or folder base name
    bool isDir = false;
    bool deleteSourceAfter = false;  // For Ctrl+X (Cut & Paste)
    uint64_t totalBytes = 0;
    uint64_t transferredBytes = 0;
    double speedBytesPerSec = 0.0;
    int progressPercent = 0;
    ProtocolPreference protocolPreference = ProtocolPreference::Auto;
    RemoteToRemotePolicy r2rPolicy = RemoteToRemotePolicy::DirectThenRelay;
    TransferProtocol activeProtocol = TransferProtocol::Sftp;
    TransferStatus status = TransferStatus::Queued;
    QString statusMessage;
    QDateTime createdAt;
    QDateTime finishedAt;
    QString targetPaneId;     // Optional pane ID to auto-refresh upon completion
};

// Formatting & path helpers
QString formatBytes(uint64_t bytes);
QString formatSpeed(double bytesPerSec);
QString formatEta(uint64_t remainingBytes, double bytesPerSec);
QString formatPermissions(uint32_t mode, bool isDir, bool isSymlink);
QString protocolToString(TransferProtocol p);
QString protocolPreferenceToString(ProtocolPreference p);
QString r2rPolicyToString(RemoteToRemotePolicy p);
QString authTypeToString(AuthType a);
QString transferStatusToString(TransferStatus s);
QString directionLabel(const EndpointSpec& src, const EndpointSpec& dst, TransferProtocol proto);

QString joinRemotePath(const QString& baseDir, const QString& childName);
QString parentRemotePath(const QString& path);
QString normalizeRemotePath(const QString& path);
QString baseNameAnyPath(const QString& path);

}  // namespace xsync

Q_DECLARE_METATYPE(xsync::FileEntry)
Q_DECLARE_METATYPE(std::vector<xsync::FileEntry>)
Q_DECLARE_METATYPE(xsync::EndpointCapabilities)
Q_DECLARE_METATYPE(xsync::EndpointSpec)
Q_DECLARE_METATYPE(xsync::SessionProfile)
Q_DECLARE_METATYPE(xsync::TransferTask)
Q_DECLARE_METATYPE(xsync::TransferProtocol)
Q_DECLARE_METATYPE(xsync::TransferStatus)
