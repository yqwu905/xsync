#pragma once

#include "core/Types.h"

#include <QPointer>
#include <QTabWidget>

class QDropEvent;
class QMenu;
class QToolButton;

namespace xsync {

class FilePane;
class SessionStore;
class TransferManager;

// Multi-tab pane container (Left or Right side of MainWindow).
// Every tab can independently be a Local folder or any registered Remote SSH machine.
class PaneTabWidget : public QTabWidget {
    Q_OBJECT
public:
    explicit PaneTabWidget(const QString& sideLabel,
                           SessionStore* sessionStore,
                           TransferManager* transferMgr,
                           QWidget* parent = nullptr);

    FilePane* openLocalTab(const QString& path = {});
    FilePane* openRemoteTab(const SessionProfile& profile, const QString& path = {});
    FilePane* currentFilePane() const;
    FilePane* findPaneById(const QString& paneId) const;
    void setTabTransferPeer(PaneTabWidget* peer);

signals:
    void paneCreated(xsync::FilePane* pane);
    void openSessionManagerRequested(xsync::PaneTabWidget* targetSide);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void connectTabTitle(FilePane* pane);
    FilePane* draggedPeerPane(const QDropEvent* event) const;
    int tabInsertionIndex(const QPoint& position) const;
    void populateAddRemoteMenu();
    void onTabCloseRequested(int index);

    QString sideLabel_;
    QPointer<PaneTabWidget> tabTransferPeer_;
    SessionStore* sessionStore_ = nullptr;
    TransferManager* transferMgr_ = nullptr;
    QToolButton* addLocalBtn_ = nullptr;
    QToolButton* addRemoteBtn_ = nullptr;
    QMenu* addRemoteMenu_ = nullptr;
};

}  // namespace xsync
