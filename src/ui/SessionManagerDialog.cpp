#include "ui/SessionManagerDialog.h"

#include "core/SessionStore.h"
#include "ui/IconCache.h"
#include "ui/SessionEditDialog.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace xsync {

SessionManagerDialog::SessionManagerDialog(SessionStore* store, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(QStringLiteral("Session Manager — Registered Remote Machines"));
    resize(860, 500);
    setMinimumSize(760, 440);
    setupUi();
    rebuildTree();
    if (store_) {
        connect(store_, &SessionStore::sessionsChanged, this, &SessionManagerDialog::rebuildTree);
    }
}

void SessionManagerDialog::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // Search + Add/Import top bar (uniform 28px tier)
    auto* topBar = new QHBoxLayout();
    topBar->setSpacing(6);
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(QStringLiteral("Search saved hosts by name, IP, user, or group..."));
    searchEdit_->setClearButtonEnabled(true);
    connect(searchEdit_, &QLineEdit::textChanged, this, &SessionManagerDialog::rebuildTree);

    auto* newBtn = new QPushButton(QStringLiteral("+ Register Host"), this);
    newBtn->setMinimumWidth(116);
    connect(newBtn, &QPushButton::clicked, this, &SessionManagerDialog::onNewHost);

    auto* importBtn = new QPushButton(QStringLiteral("Import ~/.ssh/config"), this);
    importBtn->setMinimumWidth(142);
    connect(importBtn, &QPushButton::clicked, this, &SessionManagerDialog::onImportSshConfig);

    topBar->addWidget(searchEdit_, 1);
    topBar->addWidget(newBtn);
    topBar->addWidget(importBtn);
    root->addLayout(topBar);

    // Grouped session tree (uniform 28px rows, non-clipping column widths)
    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Host / Address"),
                            QStringLiteral("Auth"), QStringLiteral("Protocol Policy"),
                            QStringLiteral("Remote ↔ Remote")});
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setRootIsDecorated(true);
    tree_->header()->setHighlightSections(false);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    tree_->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    tree_->header()->setSectionResizeMode(4, QHeaderView::Interactive);
    tree_->setColumnWidth(0, 165);
    tree_->setColumnWidth(2, 86);
    tree_->setColumnWidth(3, 198);
    tree_->setColumnWidth(4, 150);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
        updateButtonStates();
    });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &SessionManagerDialog::updateButtonStates);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item && !item->data(0, Qt::UserRole).toString().isEmpty()) {
            openSelected(OpenTarget::ActivePane);
        }
    });
    root->addWidget(tree_, 1);

    // Action footer bar (uniform 30px dialog footer tier, single #primaryBtn CTA)
    auto* botBar = new QHBoxLayout();
    botBar->setSpacing(6);
    editBtn_ = new QPushButton(QStringLiteral("Edit"), this);
    editBtn_->setProperty("btnSize", QStringLiteral("dialog"));
    dupBtn_ = new QPushButton(QStringLiteral("Duplicate"), this);
    dupBtn_->setProperty("btnSize", QStringLiteral("dialog"));
    delBtn_ = new QPushButton(QStringLiteral("Delete"), this);
    delBtn_->setProperty("btnSize", QStringLiteral("dialog"));
    delBtn_->setProperty("variant", QStringLiteral("danger"));

    openLeftBtn_ = new QPushButton(QStringLiteral("Open Left"), this);
    openLeftBtn_->setProperty("btnSize", QStringLiteral("dialog"));
    openRightBtn_ = new QPushButton(QStringLiteral("Open Right"), this);
    openRightBtn_->setObjectName(QStringLiteral("primaryBtn"));
    openRightBtn_->setProperty("variant", QStringLiteral("primary"));
    openRightBtn_->setProperty("btnSize", QStringLiteral("dialog"));
    openWinBtn_ = new QPushButton(QStringLiteral("New Window"), this);
    openWinBtn_->setProperty("btnSize", QStringLiteral("dialog"));
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    closeBtn->setProperty("btnSize", QStringLiteral("dialog"));

    connect(editBtn_, &QPushButton::clicked, this, &SessionManagerDialog::onEditHost);
    connect(dupBtn_, &QPushButton::clicked, this, &SessionManagerDialog::onDuplicateHost);
    connect(delBtn_, &QPushButton::clicked, this, &SessionManagerDialog::onDeleteHost);
    connect(openLeftBtn_, &QPushButton::clicked, this, [this]() { openSelected(OpenTarget::LeftPane); });
    connect(openRightBtn_, &QPushButton::clicked, this, [this]() { openSelected(OpenTarget::RightPane); });
    connect(openWinBtn_, &QPushButton::clicked, this, [this]() { openSelected(OpenTarget::NewWindow); });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    botBar->addWidget(editBtn_);
    botBar->addWidget(dupBtn_);
    botBar->addWidget(delBtn_);
    botBar->addStretch(1);
    botBar->addWidget(openLeftBtn_);
    botBar->addWidget(openRightBtn_);
    botBar->addWidget(openWinBtn_);
    botBar->addWidget(closeBtn);
    root->addLayout(botBar);
}

void SessionManagerDialog::rebuildTree() {
    tree_->clear();
    if (!store_) {
        updateButtonStates();
        return;
    }

    const QString filter = searchEdit_->text().trimmed();
    QMap<QString, QTreeWidgetItem*> groupItems;

    for (const SessionProfile& p : store_->sessions()) {
        if (!filter.isEmpty()) {
            const bool match = p.name.contains(filter, Qt::CaseInsensitive) ||
                               p.host.contains(filter, Qt::CaseInsensitive) ||
                               p.username.contains(filter, Qt::CaseInsensitive) ||
                               p.group.contains(filter, Qt::CaseInsensitive);
            if (!match) continue;
        }
        const QString grp = p.group.isEmpty() ? QStringLiteral("Default") : p.group;
        QTreeWidgetItem* grpNode = groupItems.value(grp, nullptr);
        if (!grpNode) {
            grpNode = new QTreeWidgetItem(tree_, {grp});
            grpNode->setIcon(0, IconCache::folderIcon());
            grpNode->setSizeHint(0, QSize(-1, 28));
            QFont f = grpNode->font(0);
            f.setBold(true);
            grpNode->setFont(0, f);
            groupItems.insert(grp, grpNode);
        }

        auto* item = new QTreeWidgetItem(grpNode, {
            p.displayTitle(),
            p.displayAddress() + (p.useJumpHost ? QStringLiteral(" (via %1)").arg(p.jumpHost) : QString()),
            authTypeToString(p.authType),
            protocolPreferenceToString(p.protocolPreference),
            r2rPolicyToString(p.r2rPolicy)
        });
        item->setIcon(0, IconCache::serverIcon());
        item->setSizeHint(0, QSize(-1, 28));
        item->setData(0, Qt::UserRole, p.id);
    }
    tree_->expandAll();
    updateButtonStates();
}

void SessionManagerDialog::updateButtonStates() {
    const bool hasHost = selectedProfile().has_value();
    if (editBtn_) editBtn_->setEnabled(hasHost);
    if (dupBtn_) dupBtn_->setEnabled(hasHost);
    if (delBtn_) delBtn_->setEnabled(hasHost);
    if (openLeftBtn_) openLeftBtn_->setEnabled(hasHost);
    if (openRightBtn_) openRightBtn_->setEnabled(hasHost);
    if (openWinBtn_) openWinBtn_->setEnabled(hasHost);
}

std::optional<SessionProfile> SessionManagerDialog::selectedProfile() const {
    if (!store_ || !tree_) return std::nullopt;
    const auto selected = tree_->selectedItems();
    if (selected.isEmpty()) return std::nullopt;
    QTreeWidgetItem* item = tree_->currentItem();
    if (!item || !selected.contains(item)) {
        item = selected.first();
    }
    if (!item) return std::nullopt;
    const QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty()) return std::nullopt;
    return store_->findById(id);
}

void SessionManagerDialog::onNewHost() {
    if (!store_) return;
    SessionEditDialog dlg(SessionProfile::createNew(), store_->groups(), this);
    if (dlg.exec() == QDialog::Accepted) {
        store_->upsertSession(dlg.profile());
    }
}

void SessionManagerDialog::onEditHost() {
    auto sel = selectedProfile();
    if (!sel.has_value() || !store_) return;
    SessionEditDialog dlg(*sel, store_->groups(), this);
    if (dlg.exec() == QDialog::Accepted) {
        store_->upsertSession(dlg.profile());
    }
}

void SessionManagerDialog::onDuplicateHost() {
    auto sel = selectedProfile();
    if (!sel.has_value() || !store_) return;
    SessionProfile copy = *sel;
    copy.id = SessionProfile::createNew().id;
    copy.name = copy.displayTitle() + QStringLiteral(" (Copy)");
    store_->upsertSession(copy);
}

void SessionManagerDialog::onDeleteHost() {
    auto sel = selectedProfile();
    if (!sel.has_value() || !store_) return;
    if (QMessageBox::question(this, QStringLiteral("Delete Host"),
                              QStringLiteral("Remove registered machine '%1'?").arg(sel->displayTitle()),
                              QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
        store_->removeSession(sel->id);
    }
}

void SessionManagerDialog::onImportSshConfig() {
    if (!store_) return;
    const int count = store_->importFromSshConfig();
    if (count > 0) {
        QMessageBox::information(this, QStringLiteral("Import Complete"),
                                 QStringLiteral("Imported %1 hosts from ~/.ssh/config.").arg(count));
    } else {
        const QString custom = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select SSH Config File"), QDir::homePath());
        if (!custom.isEmpty()) {
            const int c2 = store_->importFromSshConfig(custom);
            QMessageBox::information(this, QStringLiteral("Import Complete"),
                                     QStringLiteral("Imported %1 hosts.").arg(c2));
        }
    }
}

void SessionManagerDialog::openSelected(OpenTarget target) {
    auto sel = selectedProfile();
    if (!sel.has_value()) return;
    emit openSessionRequested(*sel, target);
    accept();
}

}  // namespace xsync
