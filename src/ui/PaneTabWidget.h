#pragma once

#include "core/Types.h"

#include <QTabWidget>

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

signals:
    void paneCreated(xsync::FilePane* pane);
    void openSessionManagerRequested(xsync::PaneTabWidget* targetSide);

private:
    void populateAddRemoteMenu();
    void onTabCloseRequested(int index);

    QString sideLabel_;
    SessionStore* sessionStore_ = nullptr;
    TransferManager* transferMgr_ = nullptr;
    QToolButton* addLocalBtn_ = nullptr;
    QToolButton* addRemoteBtn_ = nullptr;
    QMenu* addRemoteMenu_ = nullptr;
};

}  // namespace xsync
