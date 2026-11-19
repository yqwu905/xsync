#include "core/SessionStore.h"

#include "core/SecretStore.h"
#include "core/ShellUtils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

namespace xsync {

namespace {

QJsonObject profileToJson(const SessionProfile& p) {
    QJsonObject o;
    o[QStringLiteral("id")] = p.id;
    o[QStringLiteral("name")] = p.name;
    o[QStringLiteral("group")] = p.group.isEmpty() ? QStringLiteral("Default") : p.group;
    o[QStringLiteral("host")] = p.host;
    o[QStringLiteral("port")] = static_cast<int>(p.port);
    o[QStringLiteral("username")] = p.username;
    o[QStringLiteral("authType")] = static_cast<int>(p.authType);
    if (!p.password.isEmpty()) {
        o[QStringLiteral("passwordEnc")] = SecretStore::protect(p.password);
    }
    o[QStringLiteral("privateKeyPath")] = p.privateKeyPath;
    if (!p.passphrase.isEmpty()) {
        o[QStringLiteral("passphraseEnc")] = SecretStore::protect(p.passphrase);
    }
    o[QStringLiteral("defaultRemotePath")] = p.defaultRemotePath;
    o[QStringLiteral("defaultLocalPath")] = p.defaultLocalPath;
    o[QStringLiteral("protocolPreference")] = static_cast<int>(p.protocolPreference);
    o[QStringLiteral("r2rPolicy")] = static_cast<int>(p.r2rPolicy);
    o[QStringLiteral("forwardAgent")] = p.forwardAgent;
    o[QStringLiteral("keepAliveSec")] = p.keepAliveSec;

    o[QStringLiteral("useJumpHost")] = p.useJumpHost;
    if (p.useJumpHost) {
        o[QStringLiteral("jumpHost")] = p.jumpHost;
        o[QStringLiteral("jumpPort")] = static_cast<int>(p.jumpPort);
        o[QStringLiteral("jumpUsername")] = p.jumpUsername;
        o[QStringLiteral("jumpAuthType")] = static_cast<int>(p.jumpAuthType);
        if (!p.jumpPassword.isEmpty()) {
            o[QStringLiteral("jumpPasswordEnc")] = SecretStore::protect(p.jumpPassword);
        }
        o[QStringLiteral("jumpPrivateKeyPath")] = p.jumpPrivateKeyPath;
        if (!p.jumpPassphrase.isEmpty()) {
            o[QStringLiteral("jumpPassphraseEnc")] = SecretStore::protect(p.jumpPassphrase);
        }
    }
    return o;
}

SessionProfile profileFromJson(const QJsonObject& o) {
    SessionProfile p = SessionProfile::createNew();
    const QString id = o.value(QStringLiteral("id")).toString();
    if (!id.isEmpty()) p.id = id;
    p.name = o.value(QStringLiteral("name")).toString();
    p.group = o.value(QStringLiteral("group")).toString(QStringLiteral("Default"));
    p.host = o.value(QStringLiteral("host")).toString();
    p.port = static_cast<uint16_t>(qBound(1, o.value(QStringLiteral("port")).toInt(22), 65535));
    p.username = o.value(QStringLiteral("username")).toString();
    p.authType = static_cast<AuthType>(qBound(0, o.value(QStringLiteral("authType")).toInt(0), 2));
    p.password = SecretStore::unprotect(o.value(QStringLiteral("passwordEnc")).toString());
    p.privateKeyPath = o.value(QStringLiteral("privateKeyPath")).toString();
    p.passphrase = SecretStore::unprotect(o.value(QStringLiteral("passphraseEnc")).toString());
    p.defaultRemotePath = o.value(QStringLiteral("defaultRemotePath")).toString();
    p.defaultLocalPath = o.value(QStringLiteral("defaultLocalPath")).toString();
    p.protocolPreference = static_cast<ProtocolPreference>(
        qBound(0, o.value(QStringLiteral("protocolPreference")).toInt(0), 3));
    p.r2rPolicy = static_cast<RemoteToRemotePolicy>(
        qBound(0, o.value(QStringLiteral("r2rPolicy")).toInt(0), 2));
    p.forwardAgent = o.value(QStringLiteral("forwardAgent")).toBool(true);
    p.keepAliveSec = qBound(0, o.value(QStringLiteral("keepAliveSec")).toInt(30), 600);

    p.useJumpHost = o.value(QStringLiteral("useJumpHost")).toBool(false);
    p.jumpHost = o.value(QStringLiteral("jumpHost")).toString();
    p.jumpPort = static_cast<uint16_t>(qBound(1, o.value(QStringLiteral("jumpPort")).toInt(22), 65535));
    p.jumpUsername = o.value(QStringLiteral("jumpUsername")).toString();
    p.jumpAuthType = static_cast<AuthType>(qBound(0, o.value(QStringLiteral("jumpAuthType")).toInt(2), 2));
    p.jumpPassword = SecretStore::unprotect(o.value(QStringLiteral("jumpPasswordEnc")).toString());
    p.jumpPrivateKeyPath = o.value(QStringLiteral("jumpPrivateKeyPath")).toString();
    p.jumpPassphrase = SecretStore::unprotect(o.value(QStringLiteral("jumpPassphraseEnc")).toString());
    return p;
}

}  // namespace

SessionStore::SessionStore(const QString& storagePath, QObject* parent)
    : QObject(parent), storagePath_(storagePath) {
    if (storagePath_.isEmpty()) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        storagePath_ = dir + QStringLiteral("/sessions.json");
    }
}

bool SessionStore::load() {
    sessions_.clear();
    QFile f(storagePath_);
    if (!f.exists()) {
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return false;

    const QJsonArray arr = doc.object().value(QStringLiteral("sessions")).toArray();
    sessions_.reserve(static_cast<size_t>(arr.size()));
    for (const QJsonValue& v : arr) {
        if (v.isObject()) {
            SessionProfile p = profileFromJson(v.toObject());
            if (p.isValid()) {
                sessions_.push_back(std::move(p));
            }
        }
    }
    emit sessionsChanged();
    return true;
}

bool SessionStore::save() const {
    QFileInfo fi(storagePath_);
    QDir().mkpath(fi.absolutePath());

    QJsonArray arr;
    for (const SessionProfile& p : sessions_) {
        arr.append(profileToJson(p));
    }
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("sessions")] = arr;

    QSaveFile f(storagePath_);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

std::optional<SessionProfile> SessionStore::findById(const QString& id) const {
    for (const SessionProfile& p : sessions_) {
        if (p.id == id) return p;
    }
    return std::nullopt;
}

QStringList SessionStore::groups() const {
    QSet<QString> set;
    set.insert(QStringLiteral("Default"));
    for (const SessionProfile& p : sessions_) {
        if (!p.group.trimmed().isEmpty()) {
            set.insert(p.group.trimmed());
        }
    }
    QStringList list = set.values();
    list.sort(Qt::CaseInsensitive);
    return list;
}

void SessionStore::upsertSession(const SessionProfile& profile) {
    SessionProfile copy = profile;
    if (copy.id.isEmpty()) {
        copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (copy.group.trimmed().isEmpty()) {
        copy.group = QStringLiteral("Default");
    }

    bool updated = false;
    for (SessionProfile& existing : sessions_) {
        if (existing.id == copy.id) {
            existing = copy;
            updated = true;
            break;
        }
    }
    if (!updated) {
        sessions_.push_back(std::move(copy));
    }
    save();
    emit sessionsChanged();
}

bool SessionStore::removeSession(const QString& id) {
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (it->id == id) {
            sessions_.erase(it);
            save();
            emit sessionsChanged();
            return true;
        }
    }
    return false;
}

int SessionStore::importFromSshConfig(const QString& sshConfigPath) {
    const QString path = sshConfigPath.isEmpty()
                             ? (QDir::homePath() + QStringLiteral("/.ssh/config"))
                             : sshConfigPath;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0;
    }
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    auto imported = ShellUtils::parseSshConfig(content);
    int added = 0;
    for (const SessionProfile& p : imported) {
        bool exists = false;
        for (const SessionProfile& cur : sessions_) {
            if (cur.name == p.name && cur.host == p.host && cur.port == p.port &&
                cur.username == p.username) {
                exists = true;
                break;
            }
        }
        if (!exists && p.isValid()) {
            sessions_.push_back(p);
            ++added;
        }
    }
    if (added > 0) {
        save();
        emit sessionsChanged();
    }
    return added;
}

}  // namespace xsync
