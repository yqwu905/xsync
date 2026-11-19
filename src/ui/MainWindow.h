#pragma once

#include "core/Types.h"
#include "ui/FileTableView.h"
#include "ui/SessionManagerDialog.h"

#include <QMainWindow>
#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QSplitter;
class QTimer;

namespace xsync {

class FilePane;
class PaneTabWidget;
class SessionStore;
class TransferManager;
class TransferQueueWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    using NewWindowFactory = std::function<MainWindow*(const SessionProfile* initialRightSession)>;

    explicit MainWindow(SessionStore* sessionStore,
                        TransferManager* transferMgr,
                        NewWindowFactory windowFactory,
                        const SessionProfile* initialRightSession = nullptr,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

    PaneTabWidget* leftTabs() const { return leftTabs_; }
    PaneTabWidget* rightTabs() const { return rightTabs_; }
    void appendLog(const QString& message);
    void applyTheme(bool dark);

private:
    void setupMenus();
    void setupToolbar();
    void setupQuickConnectBar();
    void setupCentralWorkspace();
    void setupStatusBar();

    void wirePane(FilePane* pane);
    FilePane* activePane() const;
    FilePane* oppositePane(FilePane* pane) const;

    void enqueueEntriesTransfer(const EndpointSpec& srcEp,
                                const std::vector<FileEntry>& entries,
                                bool isCut,
                                FilePane* destPane,
                                const QString& targetDir);
    void enqueueLocalPathsTransfer(const QStringList& localPaths,
                                   FilePane* destPane,
                                   const QString& targetDir);

    void openSessionManager(PaneTabWidget* preferredSide = nullptr);
    void openQuickConnect(bool openInRight);
    void saveQuickConnectHost();
    void onTransferSelectedClicked();
    void onTrimMemory();
    void updateStatusStats();
    void showAboutDialog();

    SessionStore* sessionStore_ = nullptr;
    TransferManager* transferMgr_ = nullptr;
    NewWindowFactory windowFactory_;

    PaneTabWidget* leftTabs_ = nullptr;
    PaneTabWidget* rightTabs_ = nullptr;
    FilePane* lastActivePane_ = nullptr;
    TransferQueueWidget* queueWidget_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;

    QComboBox* protoPrefCombo_ = nullptr;
    QComboBox* r2rPolicyCombo_ = nullptr;
    QAction* showHiddenAct_ = nullptr;
    QAction* darkThemeAct_ = nullptr;

    // Quick Connect bar widgets
    QLineEdit* qcHostEdit_ = nullptr;
    QSpinBox* qcPortSpin_ = nullptr;
    QLineEdit* qcUserEdit_ = nullptr;
    QLineEdit* qcPassEdit_ = nullptr;

    // Status bar widgets
    QLabel* selectionStatusLabel_ = nullptr;
    QLabel* transferStatusLabel_ = nullptr;
    QLabel* ramStatusLabel_ = nullptr;
    QTimer* ramTimer_ = nullptr;
};

}  // namespace xsync
