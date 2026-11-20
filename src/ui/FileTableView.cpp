#include "ui/FileTableView.h"

#include "core/FileSystemBackend.h"
#include "core/SecretStore.h"
#include "ui/FileListModel.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QClipboard>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QPainter>
#include <QPixmap>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QUrl>
#include <cmath>

namespace xsync {

namespace {

QJsonObject sessionToMimeJson(const SessionProfile& s) {
    QJsonObject o;
    o[QStringLiteral("id")] = s.id;
    o[QStringLiteral("name")] = s.name;
    o[QStringLiteral("group")] = s.group;
    o[QStringLiteral("host")] = s.host;
    o[QStringLiteral("port")] = static_cast<int>(s.port);
    o[QStringLiteral("username")] = s.username;
    o[QStringLiteral("authType")] = static_cast<int>(s.authType);
    if (!s.password.isEmpty()) o[QStringLiteral("pw")] = SecretStore::protect(s.password);
    o[QStringLiteral("key")] = s.privateKeyPath;
    if (!s.passphrase.isEmpty()) o[QStringLiteral("pp")] = SecretStore::protect(s.passphrase);
    o[QStringLiteral("proto")] = static_cast<int>(s.protocolPreference);
    o[QStringLiteral("r2r")] = static_cast<int>(s.r2rPolicy);
    o[QStringLiteral("fwd")] = s.forwardAgent;
    o[QStringLiteral("jump")] = s.useJumpHost;
    if (s.useJumpHost) {
        o[QStringLiteral("jHost")] = s.jumpHost;
        o[QStringLiteral("jPort")] = static_cast<int>(s.jumpPort);
        o[QStringLiteral("jUser")] = s.jumpUsername;
        o[QStringLiteral("jAuth")] = static_cast<int>(s.jumpAuthType);
        if (!s.jumpPassword.isEmpty()) o[QStringLiteral("jPw")] = SecretStore::protect(s.jumpPassword);
        o[QStringLiteral("jKey")] = s.jumpPrivateKeyPath;
        if (!s.jumpPassphrase.isEmpty()) o[QStringLiteral("jPp")] = SecretStore::protect(s.jumpPassphrase);
    }
    return o;
}

SessionProfile sessionFromMimeJson(const QJsonObject& o) {
    SessionProfile s;
    s.id = o.value(QStringLiteral("id")).toString();
    s.name = o.value(QStringLiteral("name")).toString();
    s.group = o.value(QStringLiteral("group")).toString();
    s.host = o.value(QStringLiteral("host")).toString();
    s.port = static_cast<uint16_t>(o.value(QStringLiteral("port")).toInt(22));
    s.username = o.value(QStringLiteral("username")).toString();
    s.authType = static_cast<AuthType>(o.value(QStringLiteral("authType")).toInt(0));
    s.password = SecretStore::unprotect(o.value(QStringLiteral("pw")).toString());
    s.privateKeyPath = o.value(QStringLiteral("key")).toString();
    s.passphrase = SecretStore::unprotect(o.value(QStringLiteral("pp")).toString());
    s.protocolPreference = static_cast<ProtocolPreference>(o.value(QStringLiteral("proto")).toInt(0));
    s.r2rPolicy = static_cast<RemoteToRemotePolicy>(o.value(QStringLiteral("r2r")).toInt(0));
    s.forwardAgent = o.value(QStringLiteral("fwd")).toBool(true);
    s.useJumpHost = o.value(QStringLiteral("jump")).toBool(false);
    s.jumpHost = o.value(QStringLiteral("jHost")).toString();
    s.jumpPort = static_cast<uint16_t>(o.value(QStringLiteral("jPort")).toInt(22));
    s.jumpUsername = o.value(QStringLiteral("jUser")).toString();
    s.jumpAuthType = static_cast<AuthType>(o.value(QStringLiteral("jAuth")).toInt(2));
    s.jumpPassword = SecretStore::unprotect(o.value(QStringLiteral("jPw")).toString());
    s.jumpPrivateKeyPath = o.value(QStringLiteral("jKey")).toString();
    s.jumpPassphrase = SecretStore::unprotect(o.value(QStringLiteral("jPp")).toString());
    return s;
}

}  // namespace

FileTableView::FileTableView(QWidget* parent) : QTableView(parent) {
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setAlternatingRowColors(true);
    setSortingEnabled(true);
    setShowGrid(false);
    setWordWrap(false);
    setEditTriggers(QAbstractItemView::NoEditTriggers);

    verticalHeader()->setVisible(false);
    verticalHeader()->setDefaultSectionSize(26);
    horizontalHeader()->setStretchLastSection(true);
    horizontalHeader()->setHighlightSections(false);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](bool) {
        viewport()->update();
    });

    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
    viewport()->setAcceptDrops(true);

    // Scoped Ctrl+C / Ctrl+X / Ctrl+V shortcuts so QLineEdit paste in nav/filter bars stays intact
    auto* copySc = new QShortcut(QKeySequence::Copy, this);
    copySc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copySc, &QShortcut::activated, this, [this]() { copySelectionToClipboard(false); });

    auto* cutSc = new QShortcut(QKeySequence::Cut, this);
    cutSc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(cutSc, &QShortcut::activated, this, [this]() { copySelectionToClipboard(true); });

    auto* pasteSc = new QShortcut(QKeySequence::Paste, this);
    pasteSc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(pasteSc, &QShortcut::activated, this, &FileTableView::pasteFromClipboard);
}

void FileTableView::setContextInfo(const EndpointSpec& endpoint, const QString& currentDir) {
    endpoint_ = endpoint;
    currentDir_ = currentDir;
}

std::vector<FileEntry> FileTableView::selectedEntries() const {
    std::vector<FileEntry> out;
    if (!selectionModel()) return out;
    const QModelIndexList rows = selectionModel()->selectedRows();

    const auto* proxy = qobject_cast<const QSortFilterProxyModel*>(model());
    const auto* srcModel = proxy ? qobject_cast<const FileListModel*>(proxy->sourceModel())
                                 : qobject_cast<const FileListModel*>(model());
    if (!srcModel) return out;

    out.reserve(static_cast<size_t>(rows.size()));
    for (const QModelIndex& idx : rows) {
        const QModelIndex srcIdx = proxy ? proxy->mapToSource(idx) : idx;
        if (const FileEntry* e = srcModel->entryAt(srcIdx.row())) {
            if (!e->isParentLink) {
                out.push_back(*e);
            }
        }
    }
    return out;
}

QByteArray FileTableView::encodePayload(const XsyncMimePayload& payload) {
    QJsonObject root;
    root[QStringLiteral("endpointType")] = static_cast<int>(payload.sourceEndpoint.type);
    if (payload.sourceEndpoint.isRemote()) {
        root[QStringLiteral("session")] = sessionToMimeJson(payload.sourceEndpoint.session);
    }
    root[QStringLiteral("sourceDir")] = payload.sourceDir;
    root[QStringLiteral("isCut")] = payload.isCut;

    QJsonArray arr;
    for (const FileEntry& e : payload.entries) {
        QJsonObject eo;
        eo[QStringLiteral("name")] = e.name;
        eo[QStringLiteral("fullPath")] = e.fullPath;
        eo[QStringLiteral("size")] = static_cast<qint64>(e.size);
        eo[QStringLiteral("isDir")] = e.isDir;
        eo[QStringLiteral("isSymlink")] = e.isSymlink;
        arr.append(eo);
    }
    root[QStringLiteral("entries")] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::optional<XsyncMimePayload> FileTableView::decodePayload(const QByteArray& bytes) {
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject()) return std::nullopt;
    const QJsonObject root = doc.object();

    XsyncMimePayload payload;
    payload.sourceEndpoint.type =
        static_cast<EndpointType>(root.value(QStringLiteral("endpointType")).toInt(0));
    if (payload.sourceEndpoint.isRemote()) {
        payload.sourceEndpoint.session =
            sessionFromMimeJson(root.value(QStringLiteral("session")).toObject());
    }
    payload.sourceDir = root.value(QStringLiteral("sourceDir")).toString();
    payload.isCut = root.value(QStringLiteral("isCut")).toBool(false);

    const QJsonArray arr = root.value(QStringLiteral("entries")).toArray();
    payload.entries.reserve(static_cast<size_t>(arr.size()));
    for (const QJsonValue& v : arr) {
        if (!v.isObject()) continue;
        const QJsonObject eo = v.toObject();
        FileEntry e;
        e.name = eo.value(QStringLiteral("name")).toString();
        e.fullPath = eo.value(QStringLiteral("fullPath")).toString();
        e.size = static_cast<uint64_t>(std::max<qint64>(0, eo.value(QStringLiteral("size")).toInteger()));
        e.isDir = eo.value(QStringLiteral("isDir")).toBool(false);
        e.isSymlink = eo.value(QStringLiteral("isSymlink")).toBool(false);
        if (!e.fullPath.isEmpty()) payload.entries.push_back(std::move(e));
    }
    if (payload.entries.empty()) return std::nullopt;
    return payload;
}

void FileTableView::copySelectionToClipboard(bool isCut) {
    const auto entries = selectedEntries();
    if (entries.empty()) return;

    auto* mime = new QMimeData();
    XsyncMimePayload payload;
    payload.sourceEndpoint = endpoint_;
    payload.sourceDir = currentDir_;
    payload.isCut = isCut;
    payload.entries = entries;
    mime->setData(QString::fromLatin1(kXsyncMimeType), encodePayload(payload));

    QStringList plainPaths;
    QList<QUrl> urls;
    for (const FileEntry& e : entries) {
        plainPaths << e.fullPath;
        if (endpoint_.isLocal()) {
            urls << QUrl::fromLocalFile(e.fullPath);
        }
    }
    if (!urls.isEmpty()) {
        // Enables pasting Local pane files directly into Windows Explorer / Finder / Nautilus!
        mime->setUrls(urls);
    }
    mime->setText(plainPaths.join(QLatin1Char('\n')));
    QApplication::clipboard()->setMimeData(mime);
}

void FileTableView::pasteFromClipboard() {
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (!mime) return;

    // 1. Internal xsync clipboard (supports Local<->Remote, Remote<->Remote, cross-window!)
    if (mime->hasFormat(QString::fromLatin1(kXsyncMimeType))) {
        const auto payload = decodePayload(mime->data(QString::fromLatin1(kXsyncMimeType)));
        if (payload.has_value()) {
            emit internalEntriesDropped(*payload, currentDir_);
            return;
        }
    }

    // 2. OS Explorer clipboard (Ctrl+C in Windows Explorer -> Ctrl+V into xsync Remote or Local pane!)
    if (mime->hasUrls()) {
        QStringList localPaths;
        for (const QUrl& u : mime->urls()) {
            if (u.isLocalFile()) {
                const QString lp = u.toLocalFile();
                if (!lp.isEmpty()) localPaths << lp;
            }
        }
        if (!localPaths.isEmpty()) {
            emit externalLocalFilesDropped(localPaths, currentDir_);
        }
    }
}

void FileTableView::startDrag(Qt::DropActions /*supportedActions*/) {
    const auto entries = selectedEntries();
    if (entries.empty()) return;

    auto* mime = new QMimeData();
    XsyncMimePayload payload;
    payload.sourceEndpoint = endpoint_;
    payload.sourceDir = currentDir_;
    payload.isCut = false;
    payload.entries = entries;
    mime->setData(QString::fromLatin1(kXsyncMimeType), encodePayload(payload));

    if (endpoint_.isLocal()) {
        QList<QUrl> urls;
        for (const FileEntry& e : entries) {
            urls << QUrl::fromLocalFile(e.fullPath);
        }
        mime->setUrls(urls);
    }

    auto* drag = new QDrag(this);
    drag->setMimeData(mime);

    // Compact drag badge pixmap (low memory, instant render)
    const QString label = entries.size() == 1
                              ? entries.front().name
                              : QStringLiteral("%1 items (%2)")
                                    .arg(entries.size())
                                    .arg(endpoint_.label());
    QFontMetrics fm(font());
    const int w = std::min(320, fm.horizontalAdvance(label) + 24);
    const int h = fm.height() + 12;
    const qreal dpr = (devicePixelRatioF() > 0.0) ? devicePixelRatioF() : 1.0;
    QPixmap pix(static_cast<int>(std::lround(w * dpr)), static_cast<int>(std::lround(h * dpr)));
    pix.setDevicePixelRatio(dpr);
    pix.fill(Qt::transparent);
    {
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing, true);
        const DragBadgeColors badge = ThemeManager::dragBadgeColors();
        p.setBrush(badge.bg);
        p.setPen(QPen(badge.border, 1));
        p.drawRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), 6, 6);
        p.setPen(badge.text);
        p.drawText(QRect(10, 0, w - 20, h), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(label, Qt::ElideMiddle, w - 20));
    }
    drag->setPixmap(pix);
    drag->setHotSpot(QPoint(12, h / 2));
    drag->exec(Qt::CopyAction);
}

bool FileTableView::canAcceptMime(const QMimeData* mime) const {
    if (!mime) return false;
    if (mime->hasFormat(QString::fromLatin1(kXsyncMimeType))) return true;
    if (mime->hasUrls()) {
        for (const QUrl& u : mime->urls()) {
            if (u.isLocalFile()) return true;
        }
    }
    return false;
}

QString FileTableView::resolveDropTargetDir(const QPoint& pos) const {
    const QModelIndex idx = indexAt(pos);
    if (idx.isValid()) {
        const auto* proxy = qobject_cast<const QSortFilterProxyModel*>(model());
        const auto* srcModel = proxy ? qobject_cast<const FileListModel*>(proxy->sourceModel())
                                     : qobject_cast<const FileListModel*>(model());
        if (srcModel) {
            const QModelIndex srcIdx = proxy ? proxy->mapToSource(idx) : idx;
            if (const FileEntry* e = srcModel->entryAt(srcIdx.row())) {
                if (e->isDir && !e->fullPath.isEmpty()) {
                    return e->fullPath;
                }
            }
        }
    }
    return currentDir_;
}

void FileTableView::dragEnterEvent(QDragEnterEvent* event) {
    if (canAcceptMime(event->mimeData())) {
        dragHoverActive_ = true;
        event->acceptProposedAction();
        viewport()->update();
    } else {
        event->ignore();
    }
}

void FileTableView::dragMoveEvent(QDragMoveEvent* event) {
    if (canAcceptMime(event->mimeData())) {
        dragHoverActive_ = true;
        hoveredFolderIndex_ = indexAt(event->position().toPoint());
        event->acceptProposedAction();
        viewport()->update();
    } else {
        event->ignore();
    }
}

void FileTableView::dragLeaveEvent(QDragLeaveEvent* event) {
    dragHoverActive_ = false;
    hoveredFolderIndex_ = QModelIndex();
    viewport()->update();
    QTableView::dragLeaveEvent(event);
}

void FileTableView::dropEvent(QDropEvent* event) {
    dragHoverActive_ = false;
    hoveredFolderIndex_ = QModelIndex();
    viewport()->update();

    const QMimeData* mime = event->mimeData();
    if (!canAcceptMime(mime)) {
        event->ignore();
        return;
    }

    const QString targetDir = resolveDropTargetDir(event->position().toPoint());

    // Internal xsync drop (handles cross-pane & cross-window Local/Remote transfers)
    if (mime->hasFormat(QString::fromLatin1(kXsyncMimeType))) {
        const auto payload = decodePayload(mime->data(QString::fromLatin1(kXsyncMimeType)));
        if (payload.has_value()) {
            // Avoid accidental no-op drop onto exact same pane & exact same directory
            if (event->source() == this && targetDir == currentDir_) {
                event->ignore();
                return;
            }
            emit internalEntriesDropped(*payload, targetDir);
            event->acceptProposedAction();
            return;
        }
    }

    // External OS Explorer drop
    if (mime->hasUrls()) {
        QStringList localPaths;
        for (const QUrl& u : mime->urls()) {
            if (u.isLocalFile()) {
                const QString lp = u.toLocalFile();
                if (!lp.isEmpty()) localPaths << lp;
            }
        }
        if (!localPaths.isEmpty()) {
            emit externalLocalFilesDropped(localPaths, targetDir);
            event->acceptProposedAction();
            return;
        }
    }

    event->ignore();
}

void FileTableView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete) {
        emit deleteKeyPressed();
        return;
    }
    if (event->key() == Qt::Key_F2) {
        emit renameKeyPressed();
        return;
    }
    if (event->key() == Qt::Key_Backspace) {
        emit backspaceKeyPressed();
        return;
    }
    QTableView::keyPressEvent(event);
}

void FileTableView::paintEvent(QPaintEvent* event) {
    QTableView::paintEvent(event);
    if (dragHoverActive_) {
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor accent = ThemeManager::dropAccentColor();

        if (hoveredFolderIndex_.isValid()) {
            const auto* proxy = qobject_cast<const QSortFilterProxyModel*>(model());
            const auto* srcModel = proxy ? qobject_cast<const FileListModel*>(proxy->sourceModel())
                                         : qobject_cast<const FileListModel*>(model());
            if (srcModel) {
                const QModelIndex srcIdx = proxy ? proxy->mapToSource(hoveredFolderIndex_) : hoveredFolderIndex_;
                if (const FileEntry* e = srcModel->entryAt(srcIdx.row()); e && e->isDir) {
                    const QRect cellRect = visualRect(hoveredFolderIndex_);
                    QRect rowRect(2, cellRect.top(), viewport()->width() - 4, cellRect.height());
                    p.fillRect(rowRect, ThemeManager::dropFolderFillColor());
                    p.setPen(QPen(accent, 1));
                    p.drawRoundedRect(rowRect.adjusted(0, 0, -1, -1), 3, 3);
                }
            }
        }

        p.setPen(QPen(accent, 2, Qt::DashLine));
        p.drawRect(viewport()->rect().adjusted(2, 2, -2, -2));
    }
}

}  // namespace xsync
