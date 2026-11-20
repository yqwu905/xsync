#include "ui/MainWindow.h"

#include "core/MemoryStats.h"
#include "core/ProtocolNegotiator.h"
#include "core/SessionStore.h"
#include "core/TransferManager.h"
#include "ui/FilePane.h"
#include "ui/IconCache.h"
#include "ui/PaneTabWidget.h"
#include "ui/SessionEditDialog.h"
#include "ui/ThemeManager.h"
#include "ui/TransferQueueWidget.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <libssh2.h>

namespace xsync {

MainWindow::MainWindow(SessionStore* sessionStore,
                       TransferManager* transferMgr,
                       NewWindowFactory windowFactory,
                       const SessionProfile* initialRightSession,
                       QWidget* parent)
    : QMainWindow(parent),
      sessionStore_(sessionStore),
      transferMgr_(transferMgr),
      windowFactory_(std::move(windowFactory)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("xsync — Lightweight rsync / SFTP / SCP Client"));
    setWindowIcon(IconCache::appIcon());
    resize(1340, 840);

    setupMenus();
    setupToolbar();
    setupQuickConnectBar();
    setupCentralWorkspace();
    setupStatusBar();
    ThemeManager::polishTree(this);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](bool dark) {
        if (darkThemeAct_) {
            QSignalBlocker blocker(darkThemeAct_);
            darkThemeAct_->setChecked(dark);
        }
    });

    // Wire shared TransferManager signals
    if (transferMgr_) {
        connect(transferMgr_, &TransferManager::logMessage, this, &MainWindow::appendLog);
        connect(transferMgr_, &TransferManager::queueStatsChanged, this, &MainWindow::updateStatusStats);
        connect(transferMgr_, &TransferManager::paneRefreshRequested, this, [this](const QString& paneId) {
            if (FilePane* lp = leftTabs_->findPaneById(paneId)) lp->refresh();
            if (FilePane* rp = rightTabs_->findPaneById(paneId)) rp->refresh();
        });
    }

    // Open initial panes
    FilePane* leftPane = leftTabs_->openLocalTab();
    lastActivePane_ = leftPane;
    if (initialRightSession && initialRightSession->isValid()) {
        rightTabs_->openRemoteTab(*initialRightSession);
    } else {
        rightTabs_->openLocalTab();
    }

    const EndpointCapabilities localCaps = ProtocolNegotiator::probeLocal();
    appendLog(QStringLiteral("xsync ready | Local capabilities: %1 | Strategy: rsync → SFTP → SCP | Drag&Drop & Ctrl+C/V enabled")
                  .arg(localCaps.summaryBadge()));

    ramTimer_ = new QTimer(this);
    connect(ramTimer_, &QTimer::timeout, this, &MainWindow::updateStatusStats);
    ramTimer_->start(2000);
    updateStatusStats();
}

MainWindow::~MainWindow() = default;

void MainWindow::setupMenus() {
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("Session Manager..."), QKeySequence(QStringLiteral("Ctrl+O")),
                        this, [this]() { openSessionManager(); });
    fileMenu->addAction(QStringLiteral("Register New Host..."), QKeySequence(QStringLiteral("Ctrl+Shift+H")),
                        this, [this]() {
                            if (!sessionStore_) return;
                            SessionEditDialog dlg(SessionProfile::createNew(), sessionStore_->groups(), this);
                            if (dlg.exec() == QDialog::Accepted) {
                                sessionStore_->upsertSession(dlg.profile());
                            }
                        });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("New Window"), QKeySequence::New, this, [this]() {
        if (windowFactory_) windowFactory_(nullptr);
    });
    fileMenu->addAction(QStringLiteral("New Local Tab (Active Side)"), QKeySequence::AddTab, this, [this]() {
        PaneTabWidget* target = (lastActivePane_ && rightTabs_->indexOf(lastActivePane_) >= 0)
                                    ? rightTabs_
                                    : leftTabs_;
        target->openLocalTab();
    });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Close Window"), QKeySequence::Close, this, &QWidget::close);
    fileMenu->addAction(QStringLiteral("Quit All Windows"), QKeySequence::Quit, qApp, &QApplication::quit);

    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    editMenu->addAction(QStringLiteral("Copy Selected"), this, [this]() {
        if (FilePane* p = activePane()) p->copySelection(false);
    });
    editMenu->addAction(QStringLiteral("Cut Selected"), this, [this]() {
        if (FilePane* p = activePane()) p->copySelection(true);
    });
    editMenu->addAction(QStringLiteral("Paste into Active Pane"), this, [this]() {
        if (FilePane* p = activePane()) p->pasteClipboard();
    });
    editMenu->addSeparator();
    editMenu->addAction(QStringLiteral("New Folder..."), QKeySequence(QStringLiteral("Ctrl+Shift+N")),
                        this, [this]() {
                            if (FilePane* p = activePane()) p->createNewFolder();
                        });
    editMenu->addAction(QStringLiteral("Rename"), this, [this]() {
        if (FilePane* p = activePane()) p->renameSelected();
    });
    editMenu->addAction(QStringLiteral("Delete"), this, [this]() {
        if (FilePane* p = activePane()) p->deleteSelected();
    });

    auto* transferMenu = menuBar()->addMenu(QStringLiteral("&Transfer"));
    transferMenu->addAction(QStringLiteral("Transfer Selected to Opposite Pane"),
                            QKeySequence(QStringLiteral("Ctrl+Return")),
                            this, &MainWindow::onTransferSelectedClicked);
    transferMenu->addAction(QStringLiteral("Retry All Failed"), this, [this]() {
        if (transferMgr_) transferMgr_->retryAllFailed();
    });
    transferMenu->addAction(QStringLiteral("Clear Completed History"), this, [this]() {
        if (transferMgr_) transferMgr_->clearFinished();
    });

    auto* bmMenu = menuBar()->addMenu(QStringLiteral("&Bookmarks"));
    bmMenu->addAction(QStringLiteral("Bookmark Current Path..."), QKeySequence(QStringLiteral("Ctrl+D")),
                      this, [this]() {
                          if (FilePane* p = activePane()) p->bookmarkPath(p->currentPath());
                      });
    bmMenu->addAction(QStringLiteral("Show Host Bookmarks"), this, [this]() {
        if (FilePane* p = activePane()) p->showBookmarksMenu();
    });
    bmMenu->addAction(QStringLiteral("Manage Host Bookmarks..."), this, [this]() {
        if (FilePane* p = activePane()) p->openBookmarkManager();
    });

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->addAction(QStringLiteral("Refresh Active Pane"), QKeySequence::Refresh, this, [this]() {
        if (FilePane* p = activePane()) p->refresh();
    });
    showHiddenAct_ = viewMenu->addAction(QStringLiteral("Show Hidden Files"));
    showHiddenAct_->setCheckable(true);
    showHiddenAct_->setChecked(true);
    showHiddenAct_->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    connect(showHiddenAct_, &QAction::toggled, this, [this](bool show) {
        for (int i = 0; i < leftTabs_->count(); ++i) {
            if (auto* p = qobject_cast<FilePane*>(leftTabs_->widget(i))) p->setShowHidden(show);
        }
        for (int i = 0; i < rightTabs_->count(); ++i) {
            if (auto* p = qobject_cast<FilePane*>(rightTabs_->widget(i))) p->setShowHidden(show);
        }
    });

    viewMenu->addSeparator();
    darkThemeAct_ = viewMenu->addAction(QStringLiteral("Dark Theme"));
    darkThemeAct_->setCheckable(true);
    darkThemeAct_->setChecked(ThemeManager::isDark());
    connect(darkThemeAct_, &QAction::toggled, this, &MainWindow::applyTheme);

    viewMenu->addSeparator();
    viewMenu->addAction(QStringLiteral("Trim Process Memory / Release Caches"),
                        this, &MainWindow::onTrimMemory);

    auto* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    helpMenu->addAction(QStringLiteral("About xsync..."), this, &MainWindow::showAboutDialog);
}

void MainWindow::setupToolbar() {
    auto* tb = addToolBar(QStringLiteral("Main"));
    tb->setMovable(false);
    tb->setIconSize(QSize(16, 16));
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    QAction* actSessions = tb->addAction(IconCache::serverIcon(), QStringLiteral("Sessions"));
    actSessions->setToolTip(QStringLiteral("Open Session Manager (Ctrl+O)"));
    connect(actSessions, &QAction::triggered, this, [this]() { openSessionManager(); });

    QAction* actNewWin = tb->addAction(IconCache::newWindowIcon(), QStringLiteral("New Window"));
    actNewWin->setToolTip(QStringLiteral("Open an independent xsync window for multi-window transfers (Ctrl+N)"));
    connect(actNewWin, &QAction::triggered, this, [this]() {
        if (windowFactory_) windowFactory_(nullptr);
    });

    tb->addSeparator();

    QAction* actTransfer = tb->addAction(IconCache::transferIcon(), QStringLiteral("Transfer Selected"));
    actTransfer->setToolTip(QStringLiteral("Transfer selected items to the opposite pane (Ctrl+Enter)"));
    connect(actTransfer, &QAction::triggered, this, &MainWindow::onTransferSelectedClicked);

    QAction* actRefresh = tb->addAction(IconCache::refreshIcon(), QStringLiteral("Refresh"));
    actRefresh->setToolTip(QStringLiteral("Refresh active pane (F5)"));
    connect(actRefresh, &QAction::triggered, this, [this]() {
        if (FilePane* p = activePane()) p->refresh();
    });

    tb->addSeparator();

    auto* protoLbl = new QLabel(QStringLiteral("Protocol:"), tb);
    protoLbl->setObjectName(QStringLiteral("toolbarLabel"));
    tb->addWidget(protoLbl);

    protoPrefCombo_ = new QComboBox(tb);
    protoPrefCombo_->setObjectName(QStringLiteral("protoPrefCombo"));
    protoPrefCombo_->addItem(protocolPreferenceToString(ProtocolPreference::Auto),
                             static_cast<int>(ProtocolPreference::Auto));
    protoPrefCombo_->addItem(protocolPreferenceToString(ProtocolPreference::Rsync),
                             static_cast<int>(ProtocolPreference::Rsync));
    protoPrefCombo_->addItem(protocolPreferenceToString(ProtocolPreference::Sftp),
                             static_cast<int>(ProtocolPreference::Sftp));
    protoPrefCombo_->addItem(protocolPreferenceToString(ProtocolPreference::Scp),
                             static_cast<int>(ProtocolPreference::Scp));
    protoPrefCombo_->setMinimumWidth(214);
    protoPrefCombo_->setToolTip(QStringLiteral("Default protocol negotiation priority"));
    tb->addWidget(protoPrefCombo_);

    auto* r2rLbl = new QLabel(QStringLiteral("Remote ↔ Remote:"), tb);
    r2rLbl->setObjectName(QStringLiteral("toolbarLabel"));
    tb->addWidget(r2rLbl);

    r2rPolicyCombo_ = new QComboBox(tb);
    r2rPolicyCombo_->setObjectName(QStringLiteral("r2rPolicyCombo"));
    r2rPolicyCombo_->addItem(r2rPolicyToString(RemoteToRemotePolicy::DirectThenRelay),
                             static_cast<int>(RemoteToRemotePolicy::DirectThenRelay));
    r2rPolicyCombo_->addItem(r2rPolicyToString(RemoteToRemotePolicy::RelayOnly),
                             static_cast<int>(RemoteToRemotePolicy::RelayOnly));
    r2rPolicyCombo_->addItem(r2rPolicyToString(RemoteToRemotePolicy::DirectOnly),
                             static_cast<int>(RemoteToRemotePolicy::DirectOnly));
    r2rPolicyCombo_->setMinimumWidth(176);
    r2rPolicyCombo_->setToolTip(QStringLiteral("Routing strategy for Remote-to-Remote transfers"));
    tb->addWidget(r2rPolicyCombo_);
}

void MainWindow::setupQuickConnectBar() {
    auto* qcBar = addToolBar(QStringLiteral("Quick Connect"));
    qcBar->setMovable(false);
    insertToolBarBreak(qcBar);

    auto* titleLbl = new QLabel(QStringLiteral("Quick Connect"), qcBar);
    titleLbl->setObjectName(QStringLiteral("toolbarTitle"));
    qcBar->addWidget(titleLbl);

    auto* qcDivider = new QFrame(qcBar);
    qcDivider->setObjectName(QStringLiteral("toolbarDivider"));
    qcDivider->setFixedSize(1, 18);
    qcBar->addWidget(qcDivider);

    auto* hostLbl = new QLabel(QStringLiteral("Host:"), qcBar);
    hostLbl->setObjectName(QStringLiteral("toolbarLabel"));
    qcBar->addWidget(hostLbl);
    qcHostEdit_ = new QLineEdit(qcBar);
    qcHostEdit_->setPlaceholderText(QStringLiteral("192.168.1.10 / host"));
    qcHostEdit_->setMinimumWidth(115);
    qcHostEdit_->setMaximumWidth(190);
    qcBar->addWidget(qcHostEdit_);

    auto* portLbl = new QLabel(QStringLiteral("Port:"), qcBar);
    portLbl->setObjectName(QStringLiteral("toolbarLabel"));
    qcBar->addWidget(portLbl);
    qcPortSpin_ = new QSpinBox(qcBar);
    qcPortSpin_->setObjectName(QStringLiteral("qcPortSpin"));
    qcPortSpin_->setRange(1, 65535);
    qcPortSpin_->setValue(22);
    qcPortSpin_->setFixedWidth(78);
    qcBar->addWidget(qcPortSpin_);

    auto* userLbl = new QLabel(QStringLiteral("User:"), qcBar);
    userLbl->setObjectName(QStringLiteral("toolbarLabel"));
    qcBar->addWidget(userLbl);
    qcUserEdit_ = new QLineEdit(qcBar);
    qcUserEdit_->setPlaceholderText(QStringLiteral("root"));
    qcUserEdit_->setMinimumWidth(72);
    qcUserEdit_->setMaximumWidth(120);
    qcBar->addWidget(qcUserEdit_);

    auto* passLbl = new QLabel(QStringLiteral("Pass:"), qcBar);
    passLbl->setObjectName(QStringLiteral("toolbarLabel"));
    qcBar->addWidget(passLbl);
    qcPassEdit_ = new QLineEdit(qcBar);
    qcPassEdit_->setEchoMode(QLineEdit::Password);
    qcPassEdit_->setPlaceholderText(QStringLiteral("Empty = SSH Agent"));
    qcPassEdit_->setMinimumWidth(90);
    qcPassEdit_->setMaximumWidth(140);
    qcBar->addWidget(qcPassEdit_);

    auto* leftBtn = new QPushButton(QStringLiteral("Open Left"), qcBar);
    leftBtn->setMinimumWidth(84);
    auto* rightBtn = new QPushButton(QStringLiteral("Open Right"), qcBar);
    rightBtn->setObjectName(QStringLiteral("primaryBtn"));
    rightBtn->setProperty("variant", QStringLiteral("primary"));
    rightBtn->setMinimumWidth(98);
    auto* saveBtn = new QPushButton(QStringLiteral("Save Host"), qcBar);
    saveBtn->setMinimumWidth(86);

    connect(leftBtn, &QPushButton::clicked, this, [this]() { openQuickConnect(false); });
    connect(rightBtn, &QPushButton::clicked, this, [this]() { openQuickConnect(true); });
    connect(qcPassEdit_, &QLineEdit::returnPressed, this, [this]() { openQuickConnect(true); });
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::saveQuickConnectHost);

    qcBar->addWidget(leftBtn);
    qcBar->addWidget(rightBtn);
    qcBar->addWidget(saveBtn);
}

void MainWindow::setupCentralWorkspace() {
    auto* central = new QWidget(this);
    auto* mainLay = new QVBoxLayout(central);
    mainLay->setContentsMargins(6, 6, 6, 4);
    mainLay->setSpacing(4);

    auto* vertSplit = new QSplitter(Qt::Vertical, central);
    vertSplit->setHandleWidth(5);

    // Top: Dual-pane multi-tab workspace
    auto* paneSplit = new QSplitter(Qt::Horizontal, vertSplit);
    paneSplit->setHandleWidth(5);
    leftTabs_ = new PaneTabWidget(QStringLiteral("Left"), sessionStore_, transferMgr_, paneSplit);
    rightTabs_ = new PaneTabWidget(QStringLiteral("Right"), sessionStore_, transferMgr_, paneSplit);

    connect(leftTabs_, &PaneTabWidget::paneCreated, this, &MainWindow::wirePane);
    connect(rightTabs_, &PaneTabWidget::paneCreated, this, &MainWindow::wirePane);
    connect(leftTabs_, &PaneTabWidget::openSessionManagerRequested, this, &MainWindow::openSessionManager);
    connect(rightTabs_, &PaneTabWidget::openSessionManagerRequested, this, &MainWindow::openSessionManager);

    paneSplit->addWidget(leftTabs_);
    paneSplit->addWidget(rightTabs_);
    paneSplit->setStretchFactor(0, 1);
    paneSplit->setStretchFactor(1, 1);
    paneSplit->setSizes({670, 670});

    // Bottom: Transfer Queue + Bounded Activity Log
    auto* bottomTabs = new QTabWidget(vertSplit);
    bottomTabs->setDocumentMode(true);
    queueWidget_ = new TransferQueueWidget(transferMgr_, bottomTabs);
    logView_ = new QPlainTextEdit(bottomTabs);
    logView_->setReadOnly(true);
    // Strict upper bound on log lines -> physically prevents memory growth over months of use
    logView_->setMaximumBlockCount(kMaxLogLines);

    bottomTabs->addTab(queueWidget_, QStringLiteral("Transfer Queue"));
    bottomTabs->addTab(logView_, QStringLiteral("Activity Log"));

    vertSplit->addWidget(paneSplit);
    vertSplit->addWidget(bottomTabs);
    vertSplit->setSizes({560, 200});

    mainLay->addWidget(vertSplit);
    setCentralWidget(central);
}

void MainWindow::setupStatusBar() {
    selectionStatusLabel_ = new QLabel(QStringLiteral("Ready — Drag & Drop or Ctrl+C / Ctrl+V supported"), this);
    transferStatusLabel_ = new QLabel(QStringLiteral("Idle"), this);

    ramStatusLabel_ = new QLabel(QStringLiteral("RAM: -- MB"), this);
    ramStatusLabel_->setObjectName(QStringLiteral("ramBadge"));
    ramStatusLabel_->setToolTip(QStringLiteral("Live process resident memory (click View -> Trim Memory to compact)"));

    statusBar()->addWidget(selectionStatusLabel_, 1);
    statusBar()->addPermanentWidget(transferStatusLabel_);
    statusBar()->addPermanentWidget(ramStatusLabel_);
}

void MainWindow::wirePane(FilePane* pane) {
    if (!pane) return;
    pane->setShowHidden(showHiddenAct_ ? showHiddenAct_->isChecked() : true);

    connect(pane, &FilePane::paneActivated, this, [this](FilePane* p) {
        lastActivePane_ = p;
    });
    connect(pane, &FilePane::selectionSummaryChanged, this, [this](const QString& s) {
        selectionStatusLabel_->setText(s);
    });
    connect(pane, &FilePane::logMessage, this, &MainWindow::appendLog);

    connect(pane, &FilePane::transferToOppositeRequested, this,
            [this](FilePane* srcPane, const std::vector<FileEntry>& entries) {
                FilePane* dstPane = oppositePane(srcPane);
                if (!dstPane) return;
                if (dstPane->endpoint().isRemote() && !dstPane->isConnected()) {
                    QMessageBox::warning(this, QStringLiteral("Not Connected"),
                                         QStringLiteral("Target remote pane is not connected yet."));
                    return;
                }
                enqueueEntriesTransfer(srcPane->endpoint(), entries, false, dstPane, dstPane->currentPath());
            });

    connect(pane, &FilePane::externalFilesDroppedToPane, this,
            [this](FilePane* dstPane, const QStringList& localPaths, const QString& targetDir) {
                enqueueLocalPathsTransfer(localPaths, dstPane, targetDir);
            });

    connect(pane, &FilePane::internalEntriesDroppedToPane, this,
            [this](FilePane* dstPane, const XsyncMimePayload& payload, const QString& targetDir) {
                enqueueEntriesTransfer(payload.sourceEndpoint, payload.entries, payload.isCut,
                                       dstPane, targetDir);
            });
}

FilePane* MainWindow::activePane() const {
    if (lastActivePane_ &&
        (leftTabs_->indexOf(lastActivePane_) >= 0 || rightTabs_->indexOf(lastActivePane_) >= 0)) {
        return lastActivePane_;
    }
    return leftTabs_->currentFilePane();
}

FilePane* MainWindow::oppositePane(FilePane* pane) const {
    if (!pane) return rightTabs_->currentFilePane();
    if (leftTabs_->indexOf(pane) >= 0) return rightTabs_->currentFilePane();
    return leftTabs_->currentFilePane();
}

void MainWindow::enqueueEntriesTransfer(const EndpointSpec& srcEp,
                                        const std::vector<FileEntry>& entries,
                                        bool isCut,
                                        FilePane* destPane,
                                        const QString& targetDir) {
    if (!transferMgr_ || !destPane || entries.empty()) return;
    if (destPane->endpoint().isRemote() && !destPane->isConnected()) {
        QMessageBox::warning(this, QStringLiteral("Target Not Connected"),
                             QStringLiteral("Cannot transfer into a disconnected remote pane."));
        return;
    }

    const auto globalProto = static_cast<ProtocolPreference>(protoPrefCombo_->currentData().toInt());
    const auto globalR2R = static_cast<RemoteToRemotePolicy>(r2rPolicyCombo_->currentData().toInt());

    const QString dstDir = targetDir.isEmpty() ? destPane->currentPath() : targetDir;

    for (const FileEntry& e : entries) {
        TransferTask task;
        task.sourceEndpoint = srcEp;
        task.destEndpoint = destPane->endpoint();
        task.sourcePath = e.fullPath;
        task.destDir = dstDir;
        task.itemName = e.name;
        task.isDir = e.isDir;
        task.totalBytes = e.size;
        task.deleteSourceAfter = isCut;
        task.targetPaneId = destPane->paneId();

        // Respect per-session preference if global selector is Auto, otherwise use toolbar selection
        task.protocolPreference = globalProto;
        if (globalProto == ProtocolPreference::Auto) {
            if (task.destEndpoint.isRemote() &&
                task.destEndpoint.session.protocolPreference != ProtocolPreference::Auto) {
                task.protocolPreference = task.destEndpoint.session.protocolPreference;
            } else if (task.sourceEndpoint.isRemote() &&
                       task.sourceEndpoint.session.protocolPreference != ProtocolPreference::Auto) {
                task.protocolPreference = task.sourceEndpoint.session.protocolPreference;
            }
        }
        task.r2rPolicy = globalR2R;

        transferMgr_->enqueueTask(std::move(task));
    }
}

void MainWindow::enqueueLocalPathsTransfer(const QStringList& localPaths,
                                           FilePane* destPane,
                                           const QString& targetDir) {
    if (localPaths.isEmpty() || !destPane) return;
    std::vector<FileEntry> entries;
    entries.reserve(static_cast<size_t>(localPaths.size()));
    for (const QString& p : localPaths) {
        QFileInfo fi(p);
        if (!fi.exists()) continue;
        FileEntry e;
        e.name = fi.fileName();
        e.fullPath = fi.absoluteFilePath();
        e.isDir = fi.isDir();
        e.size = e.isDir ? 0 : static_cast<uint64_t>(std::max<qint64>(0, fi.size()));
        entries.push_back(std::move(e));
    }
    enqueueEntriesTransfer(EndpointSpec::local(), entries, false, destPane, targetDir);
}

void MainWindow::openSessionManager(PaneTabWidget* preferredSide) {
    if (!sessionStore_) return;
    SessionManagerDialog dlg(sessionStore_, this);
    connect(&dlg, &SessionManagerDialog::openSessionRequested, this,
            [this, preferredSide](const SessionProfile& profile, SessionManagerDialog::OpenTarget target) {
                if (target == SessionManagerDialog::OpenTarget::NewWindow) {
                    if (windowFactory_) windowFactory_(&profile);
                } else if (target == SessionManagerDialog::OpenTarget::LeftPane) {
                    leftTabs_->openRemoteTab(profile);
                } else if (target == SessionManagerDialog::OpenTarget::RightPane) {
                    rightTabs_->openRemoteTab(profile);
                } else {
                    PaneTabWidget* side = preferredSide
                                              ? preferredSide
                                              : ((lastActivePane_ && leftTabs_->indexOf(lastActivePane_) >= 0)
                                                     ? leftTabs_
                                                     : rightTabs_);
                    side->openRemoteTab(profile);
                }
            });
    dlg.exec();
}

void MainWindow::openQuickConnect(bool openInRight) {
    SessionProfile p = SessionProfile::createNew();
    p.host = qcHostEdit_->text().trimmed();
    p.port = static_cast<uint16_t>(qcPortSpin_->value());
    p.username = qcUserEdit_->text().trimmed();
    p.password = qcPassEdit_->text();
    p.authType = p.password.isEmpty() ? AuthType::SshAgent : AuthType::Password;
    p.name = p.host;
    if (!p.isValid()) {
        QMessageBox::warning(this, QStringLiteral("Quick Connect"),
                             QStringLiteral("Please enter Host and Username."));
        return;
    }
    if (openInRight) rightTabs_->openRemoteTab(p);
    else leftTabs_->openRemoteTab(p);
}

void MainWindow::saveQuickConnectHost() {
    if (!sessionStore_) return;
    SessionProfile p = SessionProfile::createNew();
    p.host = qcHostEdit_->text().trimmed();
    p.port = static_cast<uint16_t>(qcPortSpin_->value());
    p.username = qcUserEdit_->text().trimmed();
    p.password = qcPassEdit_->text();
    p.authType = p.password.isEmpty() ? AuthType::SshAgent : AuthType::Password;
    p.name = p.host;

    SessionEditDialog dlg(p, sessionStore_->groups(), this);
    if (dlg.exec() == QDialog::Accepted) {
        sessionStore_->upsertSession(dlg.profile());
        appendLog(QStringLiteral("Saved remote host '%1'").arg(dlg.profile().displayTitle()));
    }
}

void MainWindow::onTransferSelectedClicked() {
    FilePane* src = activePane();
    if (!src) return;
    const auto sel = src->selectedEntries();
    if (sel.empty()) {
        statusBar()->showMessage(QStringLiteral("Select one or more files/folders first."), 3000);
        return;
    }
    FilePane* dst = oppositePane(src);
    if (!dst) return;
    enqueueEntriesTransfer(src->endpoint(), sel, false, dst, dst->currentPath());
}

void MainWindow::onTrimMemory() {
    if (transferMgr_) {
        transferMgr_->clearFinished();
    }
    MemoryStats::trimProcessMemory();
    updateStatusStats();
    appendLog(QStringLiteral("Trimmed process caches; current RSS: %1 MB")
                  .arg(MemoryStats::currentRssMegabytes(), 0, 'f', 1));
}

void MainWindow::appendLog(const QString& message) {
    if (!logView_) return;
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    logView_->appendPlainText(QStringLiteral("[%1] %2").arg(ts, message));
}

void MainWindow::applyTheme(bool dark) {
    QSettings settings(QStringLiteral("xsync"), QStringLiteral("xsync"));
    settings.setValue(QStringLiteral("ui/darkTheme"), dark);
    ThemeManager::applyTheme(dark);
}

void MainWindow::updateStatusStats() {
    if (transferMgr_) {
        const int active = transferMgr_->activeCount();
        const int queued = transferMgr_->queuedCount();
        const int failed = transferMgr_->failedCount();
        const double speed = transferMgr_->aggregateSpeedBytesPerSec();
        if (active > 0 || queued > 0 || failed > 0) {
            QString text = QStringLiteral("Active: %1 | Queued: %2 | Speed: %3")
                               .arg(active)
                               .arg(queued)
                               .arg(formatSpeed(speed));
            if (failed > 0) text += QStringLiteral(" | Failed: %1").arg(failed);
            transferStatusLabel_->setText(text);
        } else {
            transferStatusLabel_->setText(QStringLiteral("Queue Idle"));
        }
    }
    ramStatusLabel_->setText(
        QStringLiteral("RAM: %1 MB").arg(MemoryStats::currentRssMegabytes(), 0, 'f', 1));
}

void MainWindow::showAboutDialog() {
    const EndpointCapabilities local = ProtocolNegotiator::probeLocal();
    QMessageBox::about(
        this,
        QStringLiteral("About xsync"),
        QStringLiteral(
            "<h3>xsync 1.0.0</h3>"
            "<p>Lightweight, leak-free native C++17 / Qt6 alternative to Xftp.</p>"
            "<ul>"
            "<li><b>Protocols:</b> Auto-negotiates <b>rsync</b> (if both sides have it) → <b>SFTP</b> → <b>SCP</b></li>"
            "<li><b>Remote ↔ Remote:</b> Direct server-to-server rsync/scp with automatic fallback to zero-disk 256 KiB streaming relay</li>"
            "<li><b>Productivity:</b> Multi-window, multi-tab panes, Explorer Drag & Drop, Ctrl+C / Ctrl+V</li>"
            "<li><b>Engine:</b> Qt %1 + libssh2 %2 + OpenSSL</li>"
            "<li><b>Local Capabilities:</b> %3</li>"
            "<li><b>Current Process RAM:</b> %4 MB</li>"
            "</ul>")
            .arg(QString::fromLatin1(qVersion()),
                 QString::fromLatin1(LIBSSH2_VERSION),
                 local.summaryBadge())
            .arg(MemoryStats::currentRssMegabytes(), 0, 'f', 1));
}

}  // namespace xsync
