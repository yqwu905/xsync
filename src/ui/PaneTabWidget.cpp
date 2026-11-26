#include "ui/PaneTabWidget.h"

#include "core/SessionStore.h"
#include "ui/FilePane.h"
#include "ui/IconCache.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QTabBar>
#include <QToolButton>
#include <QtAlgorithms>

namespace xsync {

namespace {

const QString kPaneTabMimeType = QStringLiteral("application/x-xsync-pane-tab");

class PaneTabBar : public QTabBar {
public:
    explicit PaneTabBar(PaneTabWidget* tabs) : QTabBar(tabs), tabs_(tabs) {}

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            pressPosition_ = event->position().toPoint();
            pressedPane_ = qobject_cast<FilePane*>(tabs_->widget(tabAt(pressPosition_)));
            dragPixmap_ = {};
        }
        QTabBar::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const QPoint position = event->position().toPoint();
        if (pressedPane_ && event->buttons().testFlag(Qt::LeftButton) &&
            (position - pressPosition_).manhattanLength() >= QApplication::startDragDistance()) {
            if (dragPixmap_.isNull()) {
                const QRect tab = tabRect(tabs_->indexOf(pressedPane_)).intersected(rect());
                dragPixmap_ = grab(tab);
                dragHotSpot_ = pressPosition_ - tab.topLeft();
            }

            if (!tabs_->rect().contains(mapTo(tabs_, position))) {
                auto* mime = new QMimeData;
                mime->setData(kPaneTabMimeType, pressedPane_->paneId().toUtf8());
                auto* drag = new QDrag(this);
                drag->setMimeData(mime);
                drag->setPixmap(dragPixmap_);
                drag->setHotSpot(dragHotSpot_);

                // End QTabBar's native reorder gesture before starting the cross-pane drag.
                QMouseEvent release(QEvent::MouseButtonRelease, event->position(),
                                    event->globalPosition(), Qt::LeftButton,
                                    event->buttons() & ~Qt::LeftButton, event->modifiers());
                mouseReleaseEvent(&release);
                drag->exec(Qt::MoveAction);
                return;
            }
        }
        QTabBar::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            pressedPane_.clear();
            dragPixmap_ = {};
        }
        QTabBar::mouseReleaseEvent(event);
    }

private:
    PaneTabWidget* tabs_;
    QPointer<FilePane> pressedPane_;
    QPoint pressPosition_;
    QPixmap dragPixmap_;
    QPoint dragHotSpot_;
};

}  // namespace

PaneTabWidget::PaneTabWidget(const QString& sideLabel,
                             SessionStore* sessionStore,
                             TransferManager* transferMgr,
                             QWidget* parent)
    : QTabWidget(parent),
      sideLabel_(sideLabel),
      sessionStore_(sessionStore),
      transferMgr_(transferMgr) {
    setTabBar(new PaneTabBar(this));
    setAcceptDrops(true);
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);
    setIconSize(QSize(14, 14));
    tabBar()->setElideMode(Qt::ElideRight);
    tabBar()->setExpanding(false);

    // Corner buttons: + Local | + Remote (compact 24px subtle tier, InstantPopup chevron)
    auto* corner = new QWidget(this);
    auto* lay = new QHBoxLayout(corner);
    lay->setContentsMargins(2, 2, 4, 2);
    lay->setSpacing(4);

    addLocalBtn_ = new QToolButton(corner);
    addLocalBtn_->setObjectName(QStringLiteral("addLocalBtn"));
    addLocalBtn_->setProperty("btnSize", QStringLiteral("compact"));
    addLocalBtn_->setProperty("variant", QStringLiteral("subtle"));
    addLocalBtn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    addLocalBtn_->setIcon(IconCache::plusIcon());
    addLocalBtn_->setIconSize(QSize(12, 12));
    addLocalBtn_->setText(QStringLiteral("Local"));
    addLocalBtn_->setToolTip(QStringLiteral("Open a new Local tab in %1 pane").arg(sideLabel_));
    connect(addLocalBtn_, &QToolButton::clicked, this, [this]() { openLocalTab(); });

    addRemoteBtn_ = new QToolButton(corner);
    addRemoteBtn_->setObjectName(QStringLiteral("addRemoteBtn"));
    addRemoteBtn_->setProperty("btnSize", QStringLiteral("compact"));
    addRemoteBtn_->setProperty("variant", QStringLiteral("subtle"));
    addRemoteBtn_->setProperty("hasMenu", true);
    addRemoteBtn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    addRemoteBtn_->setIcon(IconCache::plusIcon());
    addRemoteBtn_->setIconSize(QSize(12, 12));
    addRemoteBtn_->setText(QStringLiteral("Remote"));
    addRemoteBtn_->setToolTip(QStringLiteral("Open a registered Remote server tab in %1 pane").arg(sideLabel_));

    addRemoteMenu_ = new QMenu(addRemoteBtn_);
    connect(addRemoteMenu_, &QMenu::aboutToShow, this, &PaneTabWidget::populateAddRemoteMenu);
    addRemoteBtn_->setMenu(addRemoteMenu_);
    addRemoteBtn_->setPopupMode(QToolButton::InstantPopup);

    lay->addWidget(addLocalBtn_);
    lay->addWidget(addRemoteBtn_);
    setCornerWidget(corner, Qt::TopRightCorner);

    connect(this, &QTabWidget::tabCloseRequested, this, &PaneTabWidget::onTabCloseRequested);
}

FilePane* PaneTabWidget::openLocalTab(const QString& path) {
    auto* pane = new FilePane(EndpointSpec::local(), transferMgr_, sessionStore_, path, this);
    int idx = addTab(pane, IconCache::computerIcon(), QStringLiteral("Local"));
    setCurrentIndex(idx);
    connectTabTitle(pane);
    emit paneCreated(pane);
    return pane;
}

FilePane* PaneTabWidget::openRemoteTab(const SessionProfile& profile, const QString& path) {
    auto* pane = new FilePane(EndpointSpec::remote(profile), transferMgr_, sessionStore_, path, this);
    int idx = addTab(pane, IconCache::serverIcon(), profile.displayTitle());
    setTabToolTip(idx, profile.displayAddress());
    setCurrentIndex(idx);
    connectTabTitle(pane);
    emit paneCreated(pane);
    return pane;
}

void PaneTabWidget::connectTabTitle(FilePane* pane) {
    connect(pane, &FilePane::tabTitleChanged, this, [this, pane](const QString& title) {
        int i = indexOf(pane);
        if (i >= 0) setTabText(i, title);
    });
}

void PaneTabWidget::setTabTransferPeer(PaneTabWidget* peer) {
    tabTransferPeer_ = peer;
}

FilePane* PaneTabWidget::draggedPeerPane(const QDropEvent* event) const {
    if (!tabTransferPeer_ || !event->possibleActions().testFlag(Qt::MoveAction) ||
        !event->mimeData()->hasFormat(kPaneTabMimeType)) {
        return nullptr;
    }
    return tabTransferPeer_->findPaneById(QString::fromUtf8(event->mimeData()->data(kPaneTabMimeType)));
}

void PaneTabWidget::dragEnterEvent(QDragEnterEvent* event) {
    dragMoveEvent(event);
}

void PaneTabWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (draggedPeerPane(event)) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        event->ignore();
    }
}

int PaneTabWidget::tabInsertionIndex(const QPoint& position) const {
    const QPoint barPosition = tabBar()->mapFrom(this, position);
    if (!tabBar()->rect().contains(barPosition)) return count();

    const int index = tabBar()->tabAt(barPosition);
    if (index < 0) return count();

    const bool rightHalf = barPosition.x() > tabBar()->tabRect(index).center().x();
    return index + (rightHalf != isRightToLeft());
}

void PaneTabWidget::dropEvent(QDropEvent* event) {
    auto* pane = draggedPeerPane(event);
    if (!pane) {
        event->ignore();
        return;
    }

    auto* source = tabTransferPeer_.data();
    const int sourceIndex = source->indexOf(pane);
    const QString title = source->tabText(sourceIndex);
    const QIcon icon = source->tabIcon(sourceIndex);
    const QString toolTip = source->tabToolTip(sourceIndex);
    const int destIndex = tabInsertionIndex(event->position().toPoint());

    // Reparent the existing page, keeping its file model and any live SSH connection.
    source->removeTab(sourceIndex);
    disconnect(pane, &FilePane::tabTitleChanged, source, nullptr);
    const int inserted = insertTab(destIndex, pane, icon, title);
    setTabToolTip(inserted, toolTip);
    connectTabTitle(pane);
    setCurrentIndex(inserted);
    if (source->count() == 0) source->openLocalTab();
    pane->focusFileList();

    event->setDropAction(Qt::MoveAction);
    event->accept();
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

void PaneTabWidget::populateAddRemoteMenu() {
    addRemoteMenu_->clear();
    const auto oldSubs = addRemoteMenu_->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly);
    qDeleteAll(oldSubs);
    if (sessionStore_) {
        const auto& list = sessionStore_->sessions();
        if (list.empty()) {
            QAction* empty = addRemoteMenu_->addAction(QStringLiteral("(No saved remote hosts yet)"));
            empty->setEnabled(false);
        } else {
            // Group by session.group
            QMap<QString, QMenu*> groupMenus;
            for (const SessionProfile& s : list) {
                const QString grp = s.group.isEmpty() ? QStringLiteral("Default") : s.group;
                QMenu* targetMenu = addRemoteMenu_;
                if (grp != QLatin1String("Default")) {
                    if (!groupMenus.contains(grp)) {
                        groupMenus.insert(grp, addRemoteMenu_->addMenu(IconCache::folderIcon(), grp));
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
    addRemoteMenu_->addSeparator();
    QAction* mgr = addRemoteMenu_->addAction(QStringLiteral("Open Session Manager..."));
    connect(mgr, &QAction::triggered, this, [this]() { emit openSessionManagerRequested(this); });
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
