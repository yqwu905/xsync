#include "ui/PaneTabWidget.h"

#include "core/SessionStore.h"
#include "ui/FilePane.h"
#include "ui/IconCache.h"

#include <QHBoxLayout>
#include <QMenu>
#include <QToolButton>

namespace xsync {

PaneTabWidget::PaneTabWidget(const QString& sideLabel,
                             SessionStore* sessionStore,
                             TransferManager* transferMgr,
                             QWidget* parent)
    : QTabWidget(parent),
      sideLabel_(sideLabel),
      sessionStore_(sessionStore),
      transferMgr_(transferMgr) {
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);

    // Corner buttons: + Local | + Remote
    auto* corner = new QWidget(this);
    auto* lay = new QHBoxLayout(corner);
    lay->setContentsMargins(2, 2, 4, 2);
    lay->setSpacing(4);

    addLocalBtn_ = new QToolButton(corner);
    addLocalBtn_->setText(QStringLiteral("+ Local"));
    addLocalBtn_->setToolTip(QStringLiteral("Open a new Local tab in %1 pane").arg(sideLabel_));
    connect(addLocalBtn_, &QToolButton::clicked, this, [this]() { openLocalTab(); });

    addRemoteBtn_ = new QToolButton(corner);
    addRemoteBtn_->setText(QStringLiteral("+ Remote ▾"));
    addRemoteBtn_->setToolTip(QStringLiteral("Open a registered Remote server tab in %1 pane").arg(sideLabel_));
    connect(addRemoteBtn_, &QToolButton::clicked, this, &PaneTabWidget::showAddRemoteMenu);

    lay->addWidget(addLocalBtn_);
    lay->addWidget(addRemoteBtn_);
    setCornerWidget(corner, Qt::TopRightCorner);

    connect(this, &QTabWidget::tabCloseRequested, this, &PaneTabWidget::onTabCloseRequested);
}

FilePane* PaneTabWidget::openLocalTab(const QString& path) {
    auto* pane = new FilePane(EndpointSpec::local(), transferMgr_, path, this);
    int idx = addTab(pane, IconCache::computerIcon(), QStringLiteral("Local"));
    setCurrentIndex(idx);
    connect(pane, &FilePane::tabTitleChanged, this, [this, pane](const QString& title) {
        int i = indexOf(pane);
        if (i >= 0) setTabText(i, title);
    });
    emit paneCreated(pane);
    return pane;
}

FilePane* PaneTabWidget::openRemoteTab(const SessionProfile& profile, const QString& path) {
    auto* pane = new FilePane(EndpointSpec::remote(profile), transferMgr_, path, this);
    int idx = addTab(pane, IconCache::serverIcon(), profile.displayTitle());
    setTabToolTip(idx, profile.displayAddress());
    setCurrentIndex(idx);
    connect(pane, &FilePane::tabTitleChanged, this, [this, pane](const QString& title) {
        int i = indexOf(pane);
        if (i >= 0) setTabText(i, title);
    });
    emit paneCreated(pane);
    return pane;
}

FilePane* PaneTabWidget::currentFilePane() const {
    return qobject_cast<FilePane*>(currentWidget());
}

FilePane* PaneTabWidget::findPaneById(const QString& paneId) const {
    for (int i = 0; i < count(); ++i) {
        auto* p = qobject_cast<FilePane*>(widget(i));
        if (p && p->paneId() == paneId) return p;
    }
    return nullptr;
}

void PaneTabWidget::showAddRemoteMenu() {
    QMenu menu(this);
    if (sessionStore_) {
        const auto& list = sessionStore_->sessions();
        if (list.empty()) {
            QAction* empty = menu.addAction(QStringLiteral("(No saved remote hosts yet)"));
            empty->setEnabled(false);
        } else {
            // Group by session.group
            QMap<QString, QMenu*> groupMenus;
            for (const SessionProfile& s : list) {
                const QString grp = s.group.isEmpty() ? QStringLiteral("Default") : s.group;
                QMenu* targetMenu = &menu;
                if (grp != QLatin1String("Default")) {
                    if (!groupMenus.contains(grp)) {
                        groupMenus.insert(grp, menu.addMenu(IconCache::folderIcon(), grp));
                    }
                    targetMenu = groupMenus.value(grp);
                }
                QAction* a = targetMenu->addAction(
                    IconCache::serverIcon(),
                    QStringLiteral("%1 (%2)").arg(s.displayTitle(), s.displayAddress()));
                connect(a, &QAction::triggered, this, [this, s]() { openRemoteTab(s); });
            }
        }
    }
    menu.addSeparator();
    QAction* mgr = menu.addAction(QStringLiteral("Open Session Manager..."));
    connect(mgr, &QAction::triggered, this, [this]() { emit openSessionManagerRequested(this); });

    menu.exec(addRemoteBtn_->mapToGlobal(QPoint(0, addRemoteBtn_->height())));
}

void PaneTabWidget::onTabCloseRequested(int index) {
    if (count() <= 1) {
        // Keep at least one tab open; if closing the last remote tab, swap to Local
        auto* p = qobject_cast<FilePane*>(widget(index));
        if (p && p->endpoint().isRemote()) {
            openLocalTab();
            removeTab(index);
            p->deleteLater();
        }
        return;
    }
    QWidget* w = widget(index);
    removeTab(index);
    if (w) w->deleteLater();
}

}  // namespace xsync
