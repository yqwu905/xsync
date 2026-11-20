#include "ui/FilePane.h"

#include "core/FileSystemBackend.h"
#include "core/ProtocolNegotiator.h"
#include "core/SessionStore.h"
#include "core/SshClient.h"
#include "core/TransferManager.h"
#include "ui/BookmarkManagerDialog.h"
#include "ui/FileListModel.h"
#include "ui/IconCache.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QResizeEvent>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace xsync {

namespace {

class ElidedLabel final : public QLabel {
public:
    explicit ElidedLabel(const QString& fullText, QWidget* parent = nullptr)
        : QLabel(parent), fullText_(fullText) {
        setToolTip(fullText_);
        QLabel::setText(fullText_);
    }

    QSize sizeHint() const override {
        const QMargins m = contentsMargins();
        const int w = fontMetrics().horizontalAdvance(fullText_) + m.left() + m.right() + 2;
        const int h = fontMetrics().height() + m.top() + m.bottom();
        return QSize(w, h);
    }

    QSize minimumSizeHint() const override {
        QSize sz = sizeHint();
        sz.setWidth(std::min(sz.width(), 60));
        return sz;
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        updateElidedText();
    }

    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
            updateElidedText();
        }
    }

private:
    void updateElidedText() {
        const int avail = contentsRect().width();
        if (avail <= 0) {
            if (QLabel::text() != fullText_) QLabel::setText(fullText_);
            return;
        }
        const QString elided = fontMetrics().elidedText(fullText_, Qt::ElideRight, avail);
        if (QLabel::text() != elided) {
            QLabel::setText(elided);
        }
    }

    QString fullText_;
};

}  // namespace

FilePane::FilePane(const EndpointSpec& endpoint,
                   TransferManager* transferMgr,
                   SessionStore* sessionStore,
                   const QString& initialPath,
                   QWidget* parent)
    : QWidget(parent),
      paneId_(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      endpoint_(endpoint),
      transferMgr_(transferMgr),
      sessionStore_(sessionStore) {
    setupUi();

    if (sessionStore_) {
        connect(sessionStore_, &SessionStore::bookmarksChanged, this, [this](const QString& key) {
            if (key == endpoint_.cacheKey()) {
                updateBookmarkButtonState();
            }
        });
    }

    if (endpoint_.isLocal()) {
        connected_ = true;
        capabilities_ = ProtocolNegotiator::probeLocal();
        if (transferMgr_) transferMgr_->cacheCapabilities(endpoint_, capabilities_);
        capBadgeLabel_->setText(capabilities_.summaryBadge());
        homePath_ = LocalFileSystem::defaultHomePath();
        const QString startDir = initialPath.isEmpty() ? homePath_ : LocalFileSystem::normalizePath(initialPath);
        updateHeaderUi(QStringLiteral("Ready"));
        navigateTo(startDir, false);
    } else {
        homePath_ = endpoint_.session.defaultRemotePath.trimmed();
        if (!initialPath.isEmpty()) homePath_ = initialPath;
        updateHeaderUi(QStringLiteral("Connecting..."));
        startRemoteWorker();
        reconnect();
    }
}

FilePane::~FilePane() {
    stopRemoteWorker();
}

void FilePane::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 1. Header bar (fixed 36px height so Local and Remote panes align to the exact pixel)
    auto* headerFrame = new QFrame(this);
    headerFrame->setObjectName(QStringLiteral("paneHeader"));
    headerFrame->setFixedHeight(36);
    headerFrame->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* headerLayout = new QHBoxLayout(headerFrame);
    headerLayout->setContentsMargins(8, 4, 8, 4);
    headerLayout->setSpacing(6);

    hostIconLabel_ = new QLabel(headerFrame);
    hostIconLabel_->setFixedSize(16, 16);
    hostIconLabel_->setPixmap(
        (endpoint_.isLocal() ? IconCache::computerIcon() : IconCache::serverIcon()).pixmap(16, 16));

    const QString fullTitle = endpoint_.isLocal()
                                  ? QStringLiteral("Local Computer")
                                  : QStringLiteral("%1 (%2)").arg(endpoint_.session.displayTitle(),
                                                                  endpoint_.session.displayAddress());
    hostTitleLabel_ = new ElidedLabel(fullTitle, headerFrame);
    hostTitleLabel_->setObjectName(QStringLiteral("hostTitleLabel"));
    hostTitleLabel_->setMinimumWidth(60);
    hostTitleLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    QFont tf = hostTitleLabel_->font();
    tf.setBold(true);
    hostTitleLabel_->setFont(tf);

    capBadgeLabel_ = new QLabel(QStringLiteral("Probing..."), headerFrame);
    capBadgeLabel_->setObjectName(QStringLiteral("capBadge"));
    capBadgeLabel_->setToolTip(QStringLiteral("Detected transfer protocols on this endpoint"));

    statusLabel_ = new QLabel(headerFrame);
    ThemeManager::setStatus(statusLabel_, "muted");

    reconnectBtn_ = new QToolButton(headerFrame);
    reconnectBtn_->setObjectName(QStringLiteral("reconnectBtn"));
    reconnectBtn_->setProperty("btnSize", QStringLiteral("compact"));
    reconnectBtn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    reconnectBtn_->setIcon(IconCache::refreshIcon());
    reconnectBtn_->setIconSize(QSize(13, 13));
    reconnectBtn_->setText(QStringLiteral("Reconnect"));
    reconnectBtn_->setVisible(endpoint_.isRemote());
    connect(reconnectBtn_, &QToolButton::clicked, this, &FilePane::reconnect);

    headerLayout->addWidget(hostIconLabel_);
    headerLayout->addWidget(hostTitleLabel_);
    headerLayout->addWidget(capBadgeLabel_);
    headerLayout->addStretch(1);
    headerLayout->addWidget(statusLabel_);
    headerLayout->addWidget(reconnectBtn_);
    root->addWidget(headerFrame);

    // 2. Path navigation + per-host bookmarks + quick filter bar (fixed 38px height)
    auto* navBar = new QWidget(this);
    navBar->setObjectName(QStringLiteral("paneNavBar"));
    navBar->setFixedHeight(38);
    auto* navLayout = new QHBoxLayout(navBar);
    navLayout->setContentsMargins(6, 5, 6, 5);
    navLayout->setSpacing(4);

    auto configureIconBtn = [](QToolButton* btn, const QString& objName, const QIcon& icon, const QString& tip) {
        btn->setObjectName(objName);
        btn->setProperty("btnSize", QStringLiteral("icon"));
        btn->setProperty("variant", QStringLiteral("subtle"));
        btn->setIcon(icon);
        btn->setIconSize(QSize(16, 16));
        btn->setToolTip(tip);
    };

    backBtn_ = new QToolButton(navBar);
    configureIconBtn(backBtn_, QStringLiteral("backBtn"), IconCache::arrowLeftIcon(),
                     QStringLiteral("Back (Alt+Left)"));
    backBtn_->setEnabled(false);
    connect(backBtn_, &QToolButton::clicked, this, &FilePane::goBack);

    upBtn_ = new QToolButton(navBar);
    configureIconBtn(upBtn_, QStringLiteral("upBtn"), IconCache::arrowUpIcon(),
                     QStringLiteral("Parent Directory (Backspace)"));
    connect(upBtn_, &QToolButton::clicked, this, &FilePane::goUp);

    homeBtn_ = new QToolButton(navBar);
    configureIconBtn(homeBtn_, QStringLiteral("homeBtn"), IconCache::homeIcon(),
                     QStringLiteral("Home Directory"));
    connect(homeBtn_, &QToolButton::clicked, this, &FilePane::goHome);

    pathEdit_ = new QLineEdit(navBar);
    pathEdit_->setPlaceholderText(QStringLiteral("Enter path and press Enter..."));
    connect(pathEdit_, &QLineEdit::returnPressed, this, [this]() {
        navigateTo(pathEdit_->text(), true);
    });

    starBtn_ = new QToolButton(navBar);
    configureIconBtn(starBtn_, QStringLiteral("starBtn"), IconCache::starOutlineIcon(),
                     QStringLiteral("Bookmark current path for this host (Ctrl+D)"));
    connect(starBtn_, &QToolButton::clicked, this, &FilePane::toggleBookmarkCurrentPath);

    bookmarksBtn_ = new QToolButton(navBar);
    bookmarksBtn_->setObjectName(QStringLiteral("bookmarksBtn"));
    bookmarksBtn_->setProperty("btnSize", QStringLiteral("iconDropdown"));
    bookmarksBtn_->setProperty("variant", QStringLiteral("subtle"));
    bookmarksBtn_->setProperty("hasMenu", true);
    bookmarksBtn_->setIcon(IconCache::bookmarksIcon());
    bookmarksBtn_->setIconSize(QSize(15, 15));
    bookmarksBtn_->setToolTip(QStringLiteral("Open saved path bookmarks for this host"));
    bookmarksMenu_ = new QMenu(bookmarksBtn_);
    connect(bookmarksMenu_, &QMenu::aboutToShow, this, &FilePane::populateBookmarksMenu);
    bookmarksBtn_->setMenu(bookmarksMenu_);
    bookmarksBtn_->setPopupMode(QToolButton::InstantPopup);

    filterEdit_ = new QLineEdit(navBar);
    filterEdit_->setPlaceholderText(QStringLiteral("Filter..."));
    filterEdit_->setMaximumWidth(135);
    filterEdit_->setClearButtonEnabled(true);

    refreshBtn_ = new QToolButton(navBar);
    configureIconBtn(refreshBtn_, QStringLiteral("refreshBtn"), IconCache::refreshIcon(),
                     QStringLiteral("Refresh (F5)"));
    connect(refreshBtn_, &QToolButton::clicked, this, &FilePane::refresh);

    navLayout->addWidget(backBtn_);
    navLayout->addWidget(upBtn_);
    navLayout->addWidget(homeBtn_);
    navLayout->addWidget(pathEdit_, 1);
    navLayout->addWidget(starBtn_);
    navLayout->addWidget(bookmarksBtn_);
    navLayout->addWidget(filterEdit_);
    navLayout->addWidget(refreshBtn_);
    root->addWidget(navBar);

    // 3. File table view + virtual flat model
    model_ = new FileListModel(this);
    proxy_ = new FileSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);

    tableView_ = new FileTableView(this);
    tableView_->setModel(proxy_);
    tableView_->sortByColumn(FileListModel::ColName, Qt::AscendingOrder);
    tableView_->setColumnWidth(FileListModel::ColName, 195);
    tableView_->setColumnWidth(FileListModel::ColSize, 82);
    tableView_->setColumnWidth(FileListModel::ColModified, 146);
    tableView_->setColumnWidth(FileListModel::ColPermissions, 108);
    tableView_->setContextMenuPolicy(Qt::CustomContextMenu);
    tableView_->installEventFilter(this);
    pathEdit_->installEventFilter(this);

    connect(filterEdit_, &QLineEdit::textChanged, proxy_, &FileSortFilterProxyModel::setNameFilter);
    connect(tableView_, &QTableView::doubleClicked, this, &FilePane::onRowDoubleClicked);
    connect(tableView_, &QWidget::customContextMenuRequested, this, &FilePane::showContextMenu);
    connect(tableView_, &FileTableView::deleteKeyPressed, this, &FilePane::deleteSelected);
    connect(tableView_, &FileTableView::renameKeyPressed, this, &FilePane::renameSelected);
    connect(tableView_, &FileTableView::backspaceKeyPressed, this, &FilePane::goUp);

    connect(tableView_, &FileTableView::externalLocalFilesDropped, this,
            [this](const QStringList& paths, const QString& targetDir) {
                emit externalFilesDroppedToPane(this, paths, targetDir);
            });
    connect(tableView_, &FileTableView::internalEntriesDropped, this,
            [this](const XsyncMimePayload& payload, const QString& targetDir) {
                emit internalEntriesDroppedToPane(this, payload, targetDir);
            });
    connect(tableView_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &FilePane::updateFooterSummary);

    root->addWidget(tableView_, 1);

    // 4. Compact pane footer (24px height, top hairline border via #paneFooter)
    footerLabel_ = new QLabel(QStringLiteral("0 items"), this);
    footerLabel_->setObjectName(QStringLiteral("paneFooter"));
    footerLabel_->setFixedHeight(24);
    root->addWidget(footerLabel_);
}

bool FilePane::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::FocusIn && (watched == tableView_ || watched == pathEdit_)) {
        emit paneActivated(this);
    }
    return QWidget::eventFilter(watched, event);
}

void FilePane::startRemoteWorker() {
    stopWorker_.store(false, std::memory_order_relaxed);
    cancelOp_.store(false, std::memory_order_relaxed);
    workerThread_ = std::thread([this]() {
        SshClient client;
        while (!stopWorker_.load(std::memory_order_relaxed)) {
            std::function<void(SshClient&)> op;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCv_.wait_for(lock, std::chrono::seconds(20), [this]() {
                    return stopWorker_.load(std::memory_order_relaxed) || !opQueue_.empty();
                });
                if (stopWorker_.load(std::memory_order_relaxed)) break;
                if (!opQueue_.empty()) {
                    op = std::move(opQueue_.front());
                    opQueue_.pop_front();
                }
            }
            if (op) {
                op(client);
            } else if (client.isConnected()) {
                client.sendKeepAlive();
            }
        }
        client.disconnect();
    });
}

void FilePane::stopRemoteWorker() {
    cancelOp_.store(true, std::memory_order_relaxed);
    stopWorker_.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        opQueue_.clear();
    }
    queueCv_.notify_all();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void FilePane::postRemoteOp(std::function<void(SshClient&)> op) {
    if (stopWorker_.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        // Bound pending browse ops so fast clicking never queues up unbounded work
        if (opQueue_.size() > 16) {
            opQueue_.pop_front();
        }
        opQueue_.push_back(std::move(op));
    }
    queueCv_.notify_one();
}

void FilePane::updateHeaderUi(const QString& statusText, bool isError) {
    statusLabel_->setText(statusText);
    if (isError) {
        ThemeManager::setStatus(statusLabel_, "error");
    } else if (connected_) {
        ThemeManager::setStatus(statusLabel_, "ok");
    } else {
        ThemeManager::setStatus(statusLabel_, "warn");
    }
}

void FilePane::updateFooterSummary() {
    const auto sel = selectedEntries();
    const int totalRows = std::max(0, proxy_->rowCount() - (model_->entryCount() > 0 && model_->entryAt(0)->isParentLink ? 1 : 0));
    QString text;
    if (sel.empty()) {
        text = QStringLiteral("%1 items").arg(totalRows);
    } else {
        uint64_t totalSelBytes = 0;
        for (const FileEntry& e : sel) {
            if (!e.isDir) totalSelBytes += e.size;
        }
        text = QStringLiteral("%1 items  |  Selected: %2 (%3)")
                   .arg(totalRows)
                   .arg(sel.size())
                   .arg(formatBytes(totalSelBytes));
    }
    footerLabel_->setText(text);
    emit selectionSummaryChanged(text);
}

void FilePane::applyDirectoryEntries(const QString& path, std::vector<FileEntry> entries) {
    currentPath_ = path;
    pathEdit_->setText(currentPath_);
    tableView_->setContextInfo(endpoint_, currentPath_);
    if (backBtn_) {
        backBtn_->setEnabled(!history_.empty());
    }

    bool includeUp = false;
    QString parent;
    if (endpoint_.isLocal()) {
        parent = LocalFileSystem::parentPath(currentPath_);
        includeUp = (parent != currentPath_);
    } else {
        parent = parentRemotePath(currentPath_);
        includeUp = (currentPath_ != QLatin1String("/"));
    }

    model_->setEntries(std::move(entries), includeUp, parent);
    updateBookmarkButtonState();
    updateFooterSummary();
    emit tabTitleChanged(endpoint_.label());
}

void FilePane::updateBookmarkButtonState() {
    if (!starBtn_) return;
    const bool bookmarked = sessionStore_ && !currentPath_.isEmpty() &&
                            sessionStore_->hasBookmark(endpoint_, currentPath_);
    starBtn_->setIcon(bookmarked ? IconCache::starFilledIcon() : IconCache::starOutlineIcon());
    starBtn_->setObjectName(bookmarked ? QStringLiteral("starBtnActive") : QStringLiteral("starBtn"));
    starBtn_->setProperty("active", bookmarked);
    starBtn_->setToolTip(bookmarked
                             ? QStringLiteral("Current path is bookmarked (click to remove)")
                             : QStringLiteral("Bookmark current path for this host (Ctrl+D)"));
    starBtn_->style()->unpolish(starBtn_);
    starBtn_->style()->polish(starBtn_);
    starBtn_->update();
}

void FilePane::bookmarkPath(const QString& path, const QString& suggestedName) {
    if (!sessionStore_ || path.trimmed().isEmpty()) return;
    QString defaultLabel = suggestedName.trimmed();
    if (defaultLabel.isEmpty()) {
        defaultLabel = baseNameAnyPath(path);
        if (defaultLabel.isEmpty()) defaultLabel = path;
    }

    bool ok = false;
    const QString label = QInputDialog::getText(
        this,
        QStringLiteral("Bookmark Path (%1)").arg(endpoint_.label()),
        QStringLiteral("Bookmark name for:\n%1").arg(path),
        QLineEdit::Normal,
        defaultLabel,
        &ok);
    if (!ok) return;

    PathBookmark bm;
    bm.name = label.trimmed().isEmpty() ? defaultLabel : label.trimmed();
    bm.path = path;
    sessionStore_->addBookmark(endpoint_, bm);
    updateBookmarkButtonState();
    emit logMessage(QStringLiteral("Bookmarked [%1] %2 → %3")
                        .arg(endpoint_.label(), bm.name, bm.path));
}

void FilePane::toggleBookmarkCurrentPath() {
    if (!sessionStore_ || currentPath_.isEmpty()) return;
    if (sessionStore_->hasBookmark(endpoint_, currentPath_)) {
        sessionStore_->removeBookmark(endpoint_, currentPath_);
        updateBookmarkButtonState();
        emit logMessage(QStringLiteral("Removed bookmark [%1] %2")
                            .arg(endpoint_.label(), currentPath_));
    } else {
        bookmarkPath(currentPath_);
    }
}

void FilePane::populateBookmarksMenu() {
    if (!bookmarksMenu_) return;
    bookmarksMenu_->clear();
    if (sessionStore_) {
        const auto bms = sessionStore_->bookmarksFor(endpoint_);
        if (bms.empty()) {
            QAction* empty = bookmarksMenu_->addAction(
                QStringLiteral("(No bookmarks for %1 yet)").arg(endpoint_.label()));
            empty->setEnabled(false);
        } else {
            for (const PathBookmark& bm : bms) {
                QAction* a = bookmarksMenu_->addAction(
                    IconCache::folderIcon(),
                    QStringLiteral("%1   (%2)").arg(bm.displayName(), bm.path));
                a->setToolTip(bm.path);
                connect(a, &QAction::triggered, this, [this, p = bm.path]() {
                    navigateTo(p, true);
                });
            }
        }
    }
    bookmarksMenu_->addSeparator();
    QAction* addCur = bookmarksMenu_->addAction(
        IconCache::starFilledIcon(), QStringLiteral("Bookmark Current Path...\tCtrl+D"));
    addCur->setEnabled(sessionStore_ != nullptr && !currentPath_.isEmpty());
    connect(addCur, &QAction::triggered, this, [this]() { bookmarkPath(currentPath_); });

    QAction* mgr = bookmarksMenu_->addAction(QStringLiteral("Manage Bookmarks..."));
    mgr->setEnabled(sessionStore_ != nullptr);
    connect(mgr, &QAction::triggered, this, &FilePane::openBookmarkManager);
}

void FilePane::showBookmarksMenu() {
    if (bookmarksBtn_) {
        bookmarksBtn_->showMenu();
    }
}

void FilePane::openBookmarkManager() {
    if (!sessionStore_) return;
    BookmarkManagerDialog dlg(sessionStore_, endpoint_, currentPath_, this);
    if (dlg.exec() == QDialog::Accepted && !dlg.chosenPath().isEmpty()) {
        navigateTo(dlg.chosenPath(), true);
    }
    updateBookmarkButtonState();
}

void FilePane::reconnect() {
    if (endpoint_.isLocal()) {
        refresh();
        return;
    }

    connected_ = false;
    busy_ = true;
    updateHeaderUi(QStringLiteral("Connecting..."));
    emit logMessage(QStringLiteral("Connecting to %1...").arg(endpoint_.session.displayAddress()));

    const SessionProfile prof = endpoint_.session;
    const QString desiredStart = currentPath_.isEmpty() ? homePath_ : currentPath_;

    postRemoteOp([this, prof, desiredStart](SshClient& client) {
        QString err;
        bool ok = client.connectToHost(prof, err, &cancelOp_);
        if (!ok) {
            QMetaObject::invokeMethod(
                this,
                [this, err]() {
                    busy_ = false;
                    connected_ = false;
                    updateHeaderUi(QStringLiteral("Disconnected"), true);
                    emit logMessage(QStringLiteral("Connection failed (%1): %2")
                                        .arg(endpoint_.session.displayAddress(), err));
                },
                Qt::QueuedConnection);
            return;
        }

        EndpointCapabilities caps = ProtocolNegotiator::probeRemote(client, &cancelOp_);
        QString startDir = desiredStart;
        if (startDir.isEmpty()) {
            QString hErr;
            startDir = client.resolveHomeDirectory(hErr, &cancelOp_);
        }
        startDir = normalizeRemotePath(startDir);

        std::vector<FileEntry> entries;
        QString listErr;
        bool listed = client.listDirectory(startDir, entries, listErr, &cancelOp_);
        if (!listed && startDir != QLatin1String("/")) {
            startDir = QStringLiteral("/");
            listed = client.listDirectory(startDir, entries, listErr, &cancelOp_);
        }

        QMetaObject::invokeMethod(
            this,
            [this, caps, startDir, listed, listErr, entries = std::move(entries)]() mutable {
                busy_ = false;
                connected_ = true;
                capabilities_ = caps;
                if (transferMgr_) transferMgr_->cacheCapabilities(endpoint_, caps);
                capBadgeLabel_->setText(caps.summaryBadge());
                if (homePath_.isEmpty()) homePath_ = startDir;
                updateHeaderUi(QStringLiteral("Connected"));
                emit logMessage(QStringLiteral("Connected to %1 [%2]")
                                    .arg(endpoint_.session.displayAddress(), caps.summaryBadge()));
                if (listed) {
                    applyDirectoryEntries(startDir, std::move(entries));
                } else {
                    emit logMessage(QStringLiteral("Directory listing failed: %1").arg(listErr));
                }
            },
            Qt::QueuedConnection);
    });
}

void FilePane::navigateTo(const QString& path, bool recordHistory) {
    if (path.trimmed().isEmpty()) return;

    if (endpoint_.isLocal()) {
        const QString norm = LocalFileSystem::normalizePath(path);
        std::vector<FileEntry> entries;
        QString err;
        if (!LocalFileSystem::listDirectory(norm, entries, err)) {
            QMessageBox::warning(this, QStringLiteral("Navigation Error"), err);
            pathEdit_->setText(currentPath_);
            return;
        }
        if (recordHistory && !currentPath_.isEmpty() && currentPath_ != norm) {
            if (history_.size() > 64) history_.erase(history_.begin());
            history_.push_back(currentPath_);
        }
        applyDirectoryEntries(norm, std::move(entries));
        return;
    }

    // Remote navigation
    const QString norm = normalizeRemotePath(path);
    if (!connected_) {
        currentPath_ = norm;
        reconnect();
        return;
    }

    const QString prevPath = currentPath_;
    busy_ = true;
    updateHeaderUi(QStringLiteral("Loading..."));

    postRemoteOp([this, norm, prevPath, recordHistory](SshClient& client) {
        std::vector<FileEntry> entries;
        QString err;
        bool ok = client.listDirectory(norm, entries, err, &cancelOp_);
        QMetaObject::invokeMethod(
            this,
            [this, norm, prevPath, recordHistory, ok, err, entries = std::move(entries)]() mutable {
                busy_ = false;
                if (!ok) {
                    updateHeaderUi(QStringLiteral("Error"), true);
                    pathEdit_->setText(currentPath_);
                    emit logMessage(QStringLiteral("Failed to list %1: %2").arg(norm, err));
                    return;
                }
                if (recordHistory && !prevPath.isEmpty() && prevPath != norm) {
                    if (history_.size() > 64) history_.erase(history_.begin());
                    history_.push_back(prevPath);
                }
                updateHeaderUi(QStringLiteral("Connected"));
                applyDirectoryEntries(norm, std::move(entries));
            },
            Qt::QueuedConnection);
    });
}

void FilePane::goUp() {
    if (currentPath_.isEmpty()) return;
    const QString parent = endpoint_.isLocal() ? LocalFileSystem::parentPath(currentPath_)
                                               : parentRemotePath(currentPath_);
    if (parent != currentPath_) {
        navigateTo(parent, true);
    }
}

void FilePane::goBack() {
    if (history_.empty()) return;
    QString prev = history_.back();
    history_.pop_back();
    navigateTo(prev, false);
}

void FilePane::goHome() {
    if (!homePath_.isEmpty()) {
        navigateTo(homePath_, true);
    } else if (endpoint_.isLocal()) {
        navigateTo(LocalFileSystem::defaultHomePath(), true);
    } else {
        navigateTo(QStringLiteral("/"), true);
    }
}

void FilePane::refresh() {
    if (!currentPath_.isEmpty()) {
        navigateTo(currentPath_, false);
    } else if (endpoint_.isRemote()) {
        reconnect();
    }
}

void FilePane::setShowHidden(bool show) {
    proxy_->setShowHidden(show);
    updateFooterSummary();
}

void FilePane::focusFileList() {
    tableView_->setFocus();
}

std::vector<FileEntry> FilePane::selectedEntries() const {
    return tableView_->selectedEntries();
}

void FilePane::copySelection(bool isCut) {
    tableView_->copySelectionToClipboard(isCut);
}

void FilePane::pasteClipboard() {
    tableView_->pasteFromClipboard();
}

void FilePane::onRowDoubleClicked(const QModelIndex& proxyIdx) {
    if (!proxyIdx.isValid()) return;
    const QModelIndex srcIdx = proxy_->mapToSource(proxyIdx);
    const FileEntry* e = model_->entryAt(srcIdx.row());
    if (!e) return;

    if (e->isParentLink) {
        goUp();
    } else if (e->isDir) {
        navigateTo(e->fullPath, true);
    } else {
        // Xftp behavior: double-clicking a file transfers it to the opposite pane!
        emit transferToOppositeRequested(this, {*e});
    }
}

void FilePane::createNewFolder() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("New Folder"), QStringLiteral("Folder name:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    if (endpoint_.isLocal()) {
        QString err;
        const QString full = LocalFileSystem::joinPath(currentPath_, name.trimmed());
        if (!LocalFileSystem::mkdir(full, err)) {
            QMessageBox::warning(this, QStringLiteral("Error"), err);
        } else {
            refresh();
        }
        return;
    }

    const QString full = joinRemotePath(currentPath_, name.trimmed());
    postRemoteOp([this, full](SshClient& client) {
        QString err;
        bool okMk = client.mkdir(full, 0755, err, &cancelOp_);
        QMetaObject::invokeMethod(
            this,
            [this, okMk, err]() {
                if (!okMk) QMessageBox::warning(this, QStringLiteral("Error"), err);
                else refresh();
            },
            Qt::QueuedConnection);
    });
}

void FilePane::renameSelected() {
    const auto sel = selectedEntries();
    if (sel.size() != 1) return;
    const FileEntry& target = sel.front();

    bool ok = false;
    const QString newName = QInputDialog::getText(
        this, QStringLiteral("Rename"), QStringLiteral("New name:"),
        QLineEdit::Normal, target.name, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName.trimmed() == target.name) return;

    if (endpoint_.isLocal()) {
        QString err;
        const QString dst = LocalFileSystem::joinPath(currentPath_, newName.trimmed());
        if (!LocalFileSystem::renamePath(target.fullPath, dst, err)) {
            QMessageBox::warning(this, QStringLiteral("Rename Error"), err);
        } else {
            refresh();
        }
        return;
    }

    const QString oldP = target.fullPath;
    const QString newP = joinRemotePath(currentPath_, newName.trimmed());
    postRemoteOp([this, oldP, newP](SshClient& client) {
        QString err;
        bool okRen = client.renamePath(oldP, newP, err, &cancelOp_);
        QMetaObject::invokeMethod(
            this,
            [this, okRen, err]() {
                if (!okRen) QMessageBox::warning(this, QStringLiteral("Rename Error"), err);
                else refresh();
            },
            Qt::QueuedConnection);
    });
}

void FilePane::deleteSelected() {
    const auto sel = selectedEntries();
    if (sel.empty()) return;

    const QString prompt = sel.size() == 1
                               ? QStringLiteral("Delete '%1'?").arg(sel.front().name)
                               : QStringLiteral("Delete %1 selected items?").arg(sel.size());
    if (QMessageBox::question(this, QStringLiteral("Confirm Delete"), prompt,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    if (endpoint_.isLocal()) {
        QString err;
        for (const FileEntry& e : sel) {
            if (!LocalFileSystem::removePath(e.fullPath, err)) {
                QMessageBox::warning(this, QStringLiteral("Delete Error"), err);
                break;
            }
        }
        refresh();
        return;
    }

    postRemoteOp([this, sel](SshClient& client) {
        QString err;
        bool okAll = true;
        for (const FileEntry& e : sel) {
            bool ok = (e.isDir && !e.isSymlink)
                          ? client.removeDirRecursive(e.fullPath, err, &cancelOp_)
                          : client.removeFile(e.fullPath, err, &cancelOp_);
            if (!ok) {
                okAll = false;
                break;
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, okAll, err]() {
                if (!okAll) QMessageBox::warning(this, QStringLiteral("Delete Error"), err);
                refresh();
            },
            Qt::QueuedConnection);
    });
}

void FilePane::chmodSelected() {
    if (endpoint_.isLocal()) return;
    const auto sel = selectedEntries();
    if (sel.empty()) return;

    const QString curOctal = QString::number(sel.front().permissions & 07777, 8).rightJustified(3, QLatin1Char('0'));
    bool ok = false;
    const QString modeStr = QInputDialog::getText(
        this, QStringLiteral("Change Permissions (chmod)"),
        QStringLiteral("Octal mode (e.g. 755 or 644):"), QLineEdit::Normal, curOctal, &ok);
    if (!ok || modeStr.trimmed().isEmpty()) return;

    bool parsed = false;
    uint32_t mode = modeStr.trimmed().toUInt(&parsed, 8);
    if (!parsed) {
        QMessageBox::warning(this, QStringLiteral("Invalid Mode"), QStringLiteral("Please enter a valid octal mode."));
        return;
    }

    postRemoteOp([this, sel, mode](SshClient& client) {
        QString err;
        bool okAll = true;
        for (const FileEntry& e : sel) {
            if (!client.chmodPath(e.fullPath, mode, err, &cancelOp_)) {
                okAll = false;
                break;
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, okAll, err]() {
                if (!okAll) QMessageBox::warning(this, QStringLiteral("chmod Error"), err);
                refresh();
            },
            Qt::QueuedConnection);
    });
}

void FilePane::showContextMenu(const QPoint& pos) {
    QMenu menu(this);
    const auto sel = selectedEntries();

    if (!sel.empty()) {
        QAction* actTransfer = menu.addAction(QStringLiteral("Transfer to Opposite Pane"));
        connect(actTransfer, &QAction::triggered, this, [this, sel]() {
            emit transferToOppositeRequested(this, sel);
        });
        menu.addSeparator();

        QAction* actCopy = menu.addAction(QStringLiteral("Copy\tCtrl+C"));
        connect(actCopy, &QAction::triggered, this, [this]() { copySelection(false); });
        QAction* actCut = menu.addAction(QStringLiteral("Cut\tCtrl+X"));
        connect(actCut, &QAction::triggered, this, [this]() { copySelection(true); });
    }

    QAction* actPaste = menu.addAction(QStringLiteral("Paste\tCtrl+V"));
    connect(actPaste, &QAction::triggered, this, &FilePane::pasteClipboard);
    menu.addSeparator();

    QAction* actNewFolder = menu.addAction(QStringLiteral("New Folder..."));
    connect(actNewFolder, &QAction::triggered, this, &FilePane::createNewFolder);

    if (sel.size() == 1) {
        QAction* actRename = menu.addAction(QStringLiteral("Rename\tF2"));
        connect(actRename, &QAction::triggered, this, &FilePane::renameSelected);
    }
    if (!sel.empty()) {
        QAction* actDel = menu.addAction(QStringLiteral("Delete\tDel"));
        connect(actDel, &QAction::triggered, this, &FilePane::deleteSelected);
        if (endpoint_.isRemote()) {
            QAction* actChmod = menu.addAction(QStringLiteral("Permissions (chmod)..."));
            connect(actChmod, &QAction::triggered, this, &FilePane::chmodSelected);
        }
        menu.addSeparator();
        if (sel.size() == 1 && sel.front().isDir && sessionStore_) {
            QAction* actBmDir = menu.addAction(
                IconCache::starFilledIcon(), QStringLiteral("Bookmark This Folder..."));
            connect(actBmDir, &QAction::triggered, this, [this, entry = sel.front()]() {
                bookmarkPath(entry.fullPath, entry.name);
            });
        }
        QAction* actCopyPath = menu.addAction(QStringLiteral("Copy Path"));
        connect(actCopyPath, &QAction::triggered, this, [sel]() {
            QStringList ps;
            for (const FileEntry& e : sel) ps << e.fullPath;
            QApplication::clipboard()->setText(ps.join(QLatin1Char('\n')));
        });
    }

    if (endpoint_.isLocal()) {
        menu.addSeparator();
        QAction* actOpenOs = menu.addAction(QStringLiteral("Open in System Explorer"));
        connect(actOpenOs, &QAction::triggered, this, [this]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(currentPath_));
        });
    }

    menu.addSeparator();
    QAction* actRefresh = menu.addAction(QStringLiteral("Refresh\tF5"));
    connect(actRefresh, &QAction::triggered, this, &FilePane::refresh);

    menu.exec(tableView_->viewport()->mapToGlobal(pos));
}

}  // namespace xsync
