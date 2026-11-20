#pragma once

#include "core/Types.h"

#include <QHash>
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

    // Per-host path bookmarks (works for Local, registered Remote hosts, and Quick Connect hosts)
    std::vector<PathBookmark> bookmarksFor(const EndpointSpec& ep) const;
    bool hasBookmark(const EndpointSpec& ep, const QString& path) const;
    void addBookmark(const EndpointSpec& ep, const PathBookmark& bookmark);
    bool removeBookmark(const EndpointSpec& ep, const QString& path);
    void setBookmarks(const EndpointSpec& ep, const std::vector<PathBookmark>& bookmarks);

    QString storagePath() const { return storagePath_; }

signals:
    void sessionsChanged();
    void bookmarksChanged(const QString& endpointCacheKey);

private:
    int findMatchingSessionIndex(const EndpointSpec& ep) const;
    static QString normalizeBookmarkPath(const EndpointSpec& ep, const QString& path);

    QString storagePath_;
    std::vector<SessionProfile> sessions_;
    QHash<QString, std::vector<PathBookmark>> extraBookmarks_;  // "local" or Quick Connect "user@host:port"
};

}  // namespace xsync
