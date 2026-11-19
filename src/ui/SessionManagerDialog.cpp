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
    resize(760, 480);
    setupUi();
    rebuildTree();
    if (store_) {
        connect(store_, &SessionStore::sessionsChanged, this, &SessionManagerDialog::rebuildTree);
    }
}

void SessionManagerDialog::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    // Search + Add/Import top bar
    auto* topBar = new QHBoxLayout();
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(QStringLiteral("Search saved hosts by name, IP, user, or group..."));
    searchEdit_->setClearButtonEnabled(true);
    connect(searchEdit_, &QLineEdit::textChanged, this, &SessionManagerDialog::rebuildTree);

    auto* newBtn = new QPushButton(QStringLiteral("+ Register Host"), this);
    newBtn->setObjectName(QStringLiteral("primaryBtn"));
    connect(newBtn, &QPushButton::clicked, this, &SessionManagerDialog::onNewHost);

    auto* importBtn = new QPushButton(QStringLiteral("Import ~/.ssh/config"), this);
    connect(importBtn, &QPushButton::clicked, this, &SessionManagerDialog::onImportSshConfig);

    topBar->addWidget(searchEdit_, 1);
    topBar->addWidget(newBtn);
    topBar->addWidget(importBtn);
    root->addLayout(topBar);

    // Grouped session tree
    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Host / Address"),
                            QStringLiteral("Auth"), QStringLiteral("Protocol Policy"),
                            QStringLiteral("Remote ↔ Remote")});
    tree_->setAlternatingRowColors(true);
    tree_->setRootIsDecorated(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item && !item->data(0, Qt::UserRole).toString().isEmpty()) {
            openSelected(OpenTarget::ActivePane);
        }
    });
    root->addWidget(tree_, 1);

    // Action footer bar
    auto* botBar = new QHBoxLayout();
    editBtn_ = new QPushButton(QStringLiteral("Edit"), this);
    dupBtn_ = new QPushButton(QStringLiteral("Duplicate"), this);
    delBtn_ = new QPushButton(QStringLiteral("Delete"), this);

    openLeftBtn_ = new QPushButton(QStringLiteral("Open in Left Pane"), this);
    openRightBtn_ = new QPushButton(QStringLiteral("Open in Right Pane"), this);
    openRightBtn_->setObjectName(QStringLiteral("primaryBtn"));
    openWinBtn_ = new QPushButton(QStringLiteral("Open in New Window"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);

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
    if (!store_) return;

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
        item->setData(0, Qt::UserRole, p.id);
    }
    tree_->expandAll();
}

std::optional<SessionProfile> SessionManagerDialog::selectedProfile() const {
    if (!store_) return std::nullopt;
    QTreeWidgetItem* item = tree_->currentItem();
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
