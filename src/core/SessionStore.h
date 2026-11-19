#pragma once

#include "core/Types.h"

#include <QObject>
#include <QString>
#include <optional>
#include <vector>

namespace xsync {

class SessionStore : public QObject {
    Q_OBJECT
public:
    explicit SessionStore(const QString& storagePath = {}, QObject* parent = nullptr);

    bool load();
    bool save() const;

    const std::vector<SessionProfile>& sessions() const { return sessions_; }
    std::optional<SessionProfile> findById(const QString& id) const;
    QStringList groups() const;

    void upsertSession(const SessionProfile& profile);
    bool removeSession(const QString& id);
    int importFromSshConfig(const QString& sshConfigPath = {});

    QString storagePath() const { return storagePath_; }

signals:
    void sessionsChanged();

private:
    QString storagePath_;
    std::vector<SessionProfile> sessions_;
};

}  // namespace xsync
