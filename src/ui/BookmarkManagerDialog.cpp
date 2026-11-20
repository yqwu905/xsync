#include "ui/BookmarkManagerDialog.h"

#include "core/SessionStore.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace xsync {

namespace {

bool promptBookmarkEdit(QWidget* parent,
                        const QString& title,
                        QString& nameInOut,
                        QString& pathInOut) {
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.resize(440, 140);
    auto* root = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout();

    auto* nameEdit = new QLineEdit(nameInOut, &dlg);
    nameEdit->setPlaceholderText(QStringLiteral("Label (e.g. Checkpoints / Logs)"));
    auto* pathEdit = new QLineEdit(pathInOut, &dlg);
    pathEdit->setPlaceholderText(QStringLiteral("/data/models"));

    form->addRow(QStringLiteral("Name:"), nameEdit);
    form->addRow(QStringLiteral("Path:"), pathEdit);
    root->addLayout(form);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    root->addWidget(btns);

    if (dlg.exec() != QDialog::Accepted) return false;
    const QString path = pathEdit->text().trimmed();
    if (path.isEmpty()) return false;
    QString name = nameEdit->text().trimmed();
    if (name.isEmpty()) {
        name = baseNameAnyPath(path);
        if (name.isEmpty()) name = path;
    }
    nameInOut = name;
    pathInOut = path;
    return true;
}

}  // namespace

BookmarkManagerDialog::BookmarkManagerDialog(SessionStore* store,
                                             const EndpointSpec& endpoint,
                                             const QString& currentPath,
                                             QWidget* parent)
    : QDialog(parent), store_(store), endpoint_(endpoint), currentPath_(currentPath) {
    setWindowTitle(QStringLiteral("Bookmarks — %1").arg(endpoint_.label()));
    resize(620, 400);
    if (store_) {
        items_ = store_->bookmarksFor(endpoint_);
    }
    setupUi();
    reloadTable();
}

void BookmarkManagerDialog::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    auto* info = new QLabel(
        QStringLiteral("Saved paths for <b>%1</b> (%2). Double-click any entry to navigate immediately.")
            .arg(endpoint_.label(), endpoint_.cacheKey()),
        this);
    root->addWidget(info);

    table_ = new QTableWidget(0, 2, this);
    table_->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Path")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { onOpenSelected(); });
    root->addWidget(table_, 1);

    auto* row = new QHBoxLayout();
    auto* addCurBtn = new QPushButton(QStringLiteral("+ Bookmark Current"), this);
    auto* addBtn = new QPushButton(QStringLiteral("+ Add..."), this);
    auto* editBtn = new QPushButton(QStringLiteral("Edit"), this);
    auto* upBtn = new QPushButton(QStringLiteral("↑"), this);
    auto* downBtn = new QPushButton(QStringLiteral("↓"), this);
    auto* delBtn = new QPushButton(QStringLiteral("Delete"), this);
    auto* openBtn = new QPushButton(QStringLiteral("Go To Path"), this);
    openBtn->setObjectName(QStringLiteral("primaryBtn"));
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);

    connect(addCurBtn, &QPushButton::clicked, this, [this]() { onAdd(currentPath_); });
    connect(addBtn, &QPushButton::clicked, this, [this]() { onAdd({}); });
    connect(editBtn, &QPushButton::clicked, this, &BookmarkManagerDialog::onEdit);
    connect(upBtn, &QPushButton::clicked, this, [this]() { onMove(-1); });
    connect(downBtn, &QPushButton::clicked, this, [this]() { onMove(1); });
    connect(delBtn, &QPushButton::clicked, this, &BookmarkManagerDialog::onDelete);
    connect(openBtn, &QPushButton::clicked, this, &BookmarkManagerDialog::onOpenSelected);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    addCurBtn->setEnabled(!currentPath_.trimmed().isEmpty());

    row->addWidget(addCurBtn);
    row->addWidget(addBtn);
    row->addWidget(editBtn);
    row->addWidget(upBtn);
    row->addWidget(downBtn);
    row->addWidget(delBtn);
    row->addStretch(1);
    row->addWidget(openBtn);
    row->addWidget(closeBtn);
    root->addLayout(row);
}

void BookmarkManagerDialog::reloadTable() {
    table_->setRowCount(static_cast<int>(items_.size()));
    for (size_t i = 0; i < items_.size(); ++i) {
        table_->setItem(static_cast<int>(i), 0, new QTableWidgetItem(items_[i].displayName()));
        table_->setItem(static_cast<int>(i), 1, new QTableWidgetItem(items_[i].path));
    }
}

void BookmarkManagerDialog::persistTable() {
    if (store_) {
        store_->setBookmarks(endpoint_, items_);
        items_ = store_->bookmarksFor(endpoint_);
    }
    reloadTable();
}

void BookmarkManagerDialog::onAdd(const QString& presetPath) {
    QString path = presetPath;
    QString name = path.isEmpty() ? QString() : baseNameAnyPath(path);
    if (name.isEmpty() && !path.isEmpty()) name = path;
    if (!promptBookmarkEdit(this, QStringLiteral("Add Path Bookmark"), name, path)) return;

    items_.push_back({name, path});
    persistTable();
}

void BookmarkManagerDialog::onEdit() {
    const int row = table_->currentRow();
    if (row < 0 || static_cast<size_t>(row) >= items_.size()) return;
    QString name = items_[static_cast<size_t>(row)].name;
    QString path = items_[static_cast<size_t>(row)].path;
    if (!promptBookmarkEdit(this, QStringLiteral("Edit Path Bookmark"), name, path)) return;

    items_[static_cast<size_t>(row)] = {name, path};
    persistTable();
    table_->selectRow(row);
}

void BookmarkManagerDialog::onDelete() {
    const int row = table_->currentRow();
    if (row < 0 || static_cast<size_t>(row) >= items_.size()) return;
    items_.erase(items_.begin() + row);
    persistTable();
}

void BookmarkManagerDialog::onMove(int delta) {
    const int row = table_->currentRow();
    if (row < 0) return;
    const int target = row + delta;
    if (target < 0 || static_cast<size_t>(target) >= items_.size()) return;
    std::swap(items_[static_cast<size_t>(row)], items_[static_cast<size_t>(target)]);
    persistTable();
    table_->selectRow(target);
}

void BookmarkManagerDialog::onOpenSelected() {
    const int row = table_->currentRow();
    if (row < 0 || static_cast<size_t>(row) >= items_.size()) return;
    chosenPath_ = items_[static_cast<size_t>(row)].path;
    accept();
}

}  // namespace xsync
