#pragma once

#include "core/Types.h"
#include "ui/FileTableView.h"

#include <QWidget>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace xsync {

class FileListModel;
class FileSortFilterProxyModel;
class SessionStore;
class SshClient;
class TransferManager;

// A single Local or Remote browsing tab.
// Remote tabs own a dedicated single background thread with an RAII SshClient so
// the Qt GUI thread never blocks and closing a tab frees 100% of resources immediately.
class FilePane : public QWidget {
    Q_OBJECT
public:
    explicit FilePane(const EndpointSpec& endpoint,
                      TransferManager* transferMgr,
                      SessionStore* sessionStore = nullptr,
                      const QString& initialPath = {},
                      QWidget* parent = nullptr);
    ~FilePane() override;

    QString paneId() const { return paneId_; }
    const EndpointSpec& endpoint() const { return endpoint_; }
    QString currentPath() const { return currentPath_; }
    bool isConnected() const { return connected_; }
    EndpointCapabilities capabilities() const { return capabilities_; }
    std::vector<FileEntry> selectedEntries() const;

    void navigateTo(const QString& path, bool recordHistory = true);
    void goUp();
    void goBack();
    void goHome();
    void refresh();
    void reconnect();
    void setShowHidden(bool show);
    void focusFileList();

    // Per-host path bookmarks
    void bookmarkPath(const QString& path, const QString& suggestedName = {});
    void toggleBookmarkCurrentPath();
    void showBookmarksMenu();
    void openBookmarkManager();

    // Operations invoked from context menu or toolbar
    void createNewFolder();
    void renameSelected();
    void deleteSelected();
    void chmodSelected();
    void copySelection(bool isCut);
    void pasteClipboard();

signals:
    void tabTitleChanged(const QString& title);
    void transferToOppositeRequested(xsync::FilePane* sourcePane,
                                     const std::vector<xsync::FileEntry>& entries);
    void externalFilesDroppedToPane(xsync::FilePane* destPane,
                                    const QStringList& localPaths,
                                    const QString& targetDir);
    void internalEntriesDroppedToPane(xsync::FilePane* destPane,
                                      const xsync::XsyncMimePayload& payload,
                                      const QString& targetDir);
    void logMessage(const QString& message);
    void paneActivated(xsync::FilePane* pane);
    void selectionSummaryChanged(const QString& summary);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupUi();
    void startRemoteWorker();
    void stopRemoteWorker();
    void postRemoteOp(std::function<void(SshClient&)> op);
    void updateHeaderUi(const QString& statusText, bool isError = false);
    void updateBookmarkButtonState();
    void updateFooterSummary();
    void showContextMenu(const QPoint& pos);
    void onRowDoubleClicked(const QModelIndex& proxyIdx);
    void applyDirectoryEntries(const QString& path, std::vector<FileEntry> entries);

    QString paneId_;
    EndpointSpec endpoint_;
    TransferManager* transferMgr_ = nullptr;
    SessionStore* sessionStore_ = nullptr;
    EndpointCapabilities capabilities_;

    QString currentPath_;
    QString homePath_;
    std::vector<QString> history_;
    bool connected_ = false;
    bool busy_ = false;

    // UI widgets
    QLabel* hostIconLabel_ = nullptr;
    QLabel* hostTitleLabel_ = nullptr;
    QLabel* capBadgeLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QToolButton* reconnectBtn_ = nullptr;

    QToolButton* backBtn_ = nullptr;
    QToolButton* upBtn_ = nullptr;
    QToolButton* homeBtn_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QToolButton* starBtn_ = nullptr;
    QToolButton* bookmarksBtn_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QToolButton* refreshBtn_ = nullptr;

    FileListModel* model_ = nullptr;
    FileSortFilterProxyModel* proxy_ = nullptr;
    FileTableView* tableView_ = nullptr;
    QLabel* footerLabel_ = nullptr;

    // Dedicated single-threaded remote worker (Remote panes only)
    std::atomic<bool> stopWorker_{false};
    std::atomic<bool> cancelOp_{false};
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::function<void(SshClient&)>> opQueue_;
    std::thread workerThread_;
};

}  // namespace xsync
