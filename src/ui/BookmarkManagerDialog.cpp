#include "ui/BookmarkManagerDialog.h"

#include "core/SessionStore.h"
#include "ui/IconCache.h"
#include "ui/ThemeManager.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace xsync {

// ---------------------------------------------------------------------------
// BookmarkEditDialog
// ---------------------------------------------------------------------------
BookmarkEditDialog::BookmarkEditDialog(const QString& title,
                                       const QString& initialName,
                                       const QString& initialPath,
                                       QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(title);
    resize(460, 168);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* form = new QFormLayout();
    form->setSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    nameEdit_ = new QLineEdit(initialName, this);
    nameEdit_->setPlaceholderText(QStringLiteral("Label (e.g. Checkpoints / Logs)"));
    pathEdit_ = new QLineEdit(initialPath, this);
    pathEdit_->setPlaceholderText(QStringLiteral("/data/models"));

    form->addRow(QStringLiteral("Name:"), nameEdit_);
    form->addRow(QStringLiteral("Path:"), pathEdit_);
    root->addLayout(form);
    root->addStretch(1);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ThemeManager::styleDialogButtonBox(btns);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(btns);
}

QString BookmarkEditDialog::bookmarkName() const {
    return nameEdit_ ? nameEdit_->text().trimmed() : QString();
}

QString BookmarkEditDialog::bookmarkPath() const {
    return pathEdit_ ? pathEdit_->text().trimmed() : QString();
}

namespace {

bool promptBookmarkEdit(QWidget* parent,
                        const QString& title,
                        QString& nameInOut,
                        QString& pathInOut) {
    BookmarkEditDialog dlg(title, nameInOut, pathInOut, parent);
    if (dlg.exec() != QDialog::Accepted) return false;
    const QString path = dlg.bookmarkPath();
    if (path.isEmpty()) return false;
    QString name = dlg.bookmarkName();
    if (name.isEmpty()) {
        name = baseNameAnyPath(path);
        if (name.isEmpty()) name = path;
    }
    nameInOut = name;
    pathInOut = path;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// BookmarkManagerDialog
// ---------------------------------------------------------------------------
BookmarkManagerDialog::BookmarkManagerDialog(SessionStore* store,
                                             const EndpointSpec& endpoint,
                                             const QString& currentPath,
                                             QWidget* parent)
    : QDialog(parent), store_(store), endpoint_(endpoint), currentPath_(currentPath) {
    setWindowTitle(QStringLiteral("Bookmarks — %1").arg(endpoint_.label()));
    resize(660, 420);
    setMinimumSize(580, 360);
    if (store_) {
        items_ = store_->bookmarksFor(endpoint_);
    }
    setupUi();
    reloadTable();
}

void BookmarkManagerDialog::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* info = new QLabel(
        QStringLiteral("Saved paths for <b>%1</b> (%2). Double-click any entry to navigate immediately.")
            .arg(endpoint_.label(), endpoint_.cacheKey()),
        this);
    info->setWordWrap(true);
    root->addWidget(info);

    // Top list-action toolbar (uniform 28px tier + 28x28 SVG reorder buttons)
    auto* topBar = new QHBoxLayout();
    topBar->setSpacing(6);

    addCurBtn_ = new QPushButton(QStringLiteral("+ Bookmark Current"), this);
    addCurBtn_->setEnabled(!currentPath_.trimmed().isEmpty());
    auto* addBtn = new QPushButton(QStringLiteral("+ Add..."), this);
    addBtn->setMinimumWidth(78);
    editBtn_ = new QPushButton(QStringLiteral("Edit"), this);
    editBtn_->setMinimumWidth(68);
    delBtn_ = new QPushButton(QStringLiteral("Delete"), this);
    delBtn_->setProperty("variant", QStringLiteral("danger"));
    delBtn_->setMinimumWidth(72);

    upBtn_ = new QToolButton(this);
    upBtn_->setObjectName(QStringLiteral("moveUpBtn"));
    upBtn_->setProperty("btnSize", QStringLiteral("icon"));
    upBtn_->setProperty("variant", QStringLiteral("subtle"));
    upBtn_->setIcon(IconCache::arrowUpIcon());
    upBtn_->setIconSize(QSize(16, 16));
    upBtn_->setToolTip(QStringLiteral("Move Up"));

    downBtn_ = new QToolButton(this);
    downBtn_->setObjectName(QStringLiteral("moveDownBtn"));
    downBtn_->setProperty("btnSize", QStringLiteral("icon"));
    downBtn_->setProperty("variant", QStringLiteral("subtle"));
    downBtn_->setIcon(IconCache::arrowDownIcon());
    downBtn_->setIconSize(QSize(16, 16));
    downBtn_->setToolTip(QStringLiteral("Move Down"));

    connect(addCurBtn_, &QPushButton::clicked, this, [this]() { onAdd(currentPath_); });
    connect(addBtn, &QPushButton::clicked, this, [this]() { onAdd({}); });
    connect(editBtn_, &QPushButton::clicked, this, &BookmarkManagerDialog::onEdit);
    connect(delBtn_, &QPushButton::clicked, this, &BookmarkManagerDialog::onDelete);
    connect(upBtn_, &QToolButton::clicked, this, [this]() { onMove(-1); });
    connect(downBtn_, &QToolButton::clicked, this, [this]() { onMove(1); });

    topBar->addWidget(addCurBtn_);
    topBar->addWidget(addBtn);
    topBar->addWidget(editBtn_);
    topBar->addWidget(delBtn_);
    topBar->addStretch(1);
    topBar->addWidget(upBtn_);
    topBar->addWidget(downBtn_);
    root->addLayout(topBar);

    // Gridless 28px-row bookmarks table
    table_ = new QTableWidget(0, 2, this);
    table_->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Path")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(28);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &BookmarkManagerDialog::updateButtonStates);
    connect(table_, &QTableWidget::currentCellChanged, this,
            [this](int, int, int, int) { updateButtonStates(); });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { onOpenSelected(); });
    root->addWidget(table_, 1);

    // Clean 2-button dialog footer (uniform 30px dialog tier)
    auto* footer = new QHBoxLayout();
    footer->setSpacing(8);
    openBtn_ = new QPushButton(QStringLiteral("Go To Path"), this);
    openBtn_->setObjectName(QStringLiteral("primaryBtn"));
    openBtn_->setProperty("variant", QStringLiteral("primary"));
    openBtn_->setProperty("btnSize", QStringLiteral("dialog"));
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    closeBtn->setProperty("btnSize", QStringLiteral("dialog"));

    connect(openBtn_, &QPushButton::clicked, this, &BookmarkManagerDialog::onOpenSelected);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    footer->addStretch(1);
    footer->addWidget(openBtn_);
    footer->addWidget(closeBtn);
    root->addLayout(footer);
}

void BookmarkManagerDialog::reloadTable() {
    table_->setRowCount(static_cast<int>(items_.size()));
    for (size_t i = 0; i < items_.size(); ++i) {
        table_->setItem(static_cast<int>(i), 0, new QTableWidgetItem(items_[i].displayName()));
        table_->setItem(static_cast<int>(i), 1, new QTableWidgetItem(items_[i].path));
    }
    updateButtonStates();
}

int BookmarkManagerDialog::selectedRow() const {
    if (!table_ || !table_->selectionModel()) return -1;
    const auto rows = table_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return -1;
    const int row = rows.first().row();
    return (row >= 0 && row < table_->rowCount()) ? row : -1;
}

void BookmarkManagerDialog::updateButtonStates() {
    const int row = selectedRow();
    const int count = table_ ? table_->rowCount() : 0;
    const bool hasSel = (row >= 0 && row < count);
    if (editBtn_) editBtn_->setEnabled(hasSel);
    if (delBtn_) delBtn_->setEnabled(hasSel);
    if (openBtn_) openBtn_->setEnabled(hasSel);
    if (upBtn_) upBtn_->setEnabled(hasSel && row > 0);
    if (downBtn_) downBtn_->setEnabled(hasSel && row + 1 < count);
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
    const int row = selectedRow();
    if (row < 0 || static_cast<size_t>(row) >= items_.size()) return;
    QString name = items_[static_cast<size_t>(row)].name;
    QString path = items_[static_cast<size_t>(row)].path;
    if (!promptBookmarkEdit(this, QStringLiteral("Edit Path Bookmark"), name, path)) return;

    items_[static_cast<size_t>(row)] = {name, path};
    persistTable();
    table_->selectRow(row);
}

void BookmarkManagerDialog::onDelete() {
    const int row = selectedRow();
    if (row < 0 || static_cast<size_t>(row) >= items_.size()) return;
    items_.erase(items_.begin() + row);
    persistTable();
}

void BookmarkManagerDialog::onMove(int delta) {
    const int row = selectedRow();
    if (row < 0) return;
    const int target = row + delta;
    if (target < 0 || static_cast<size_t>(target) >= items_.size()) return;
    std::swap(items_[static_cast<size_t>(row)], items_[static_cast<size_t>(target)]);
    persistTable();
    table_->selectRow(target);
}

void BookmarkManagerDialog::onOpenSelected() {
    const int row = selectedRow();
    if (row < 0 || static_cast<size_t>(row) >= items_.size()) return;
    chosenPath_ = items_[static_cast<size_t>(row)].path;
    accept();
}

}  // namespace xsync
