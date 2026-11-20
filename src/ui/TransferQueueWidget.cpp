#include "ui/TransferQueueWidget.h"

#include "core/TransferManager.h"
#include "ui/IconCache.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QSpinBox>
#include <QTreeView>
#include <QVBoxLayout>

namespace xsync {

namespace {

QColor colorForProtocol(TransferProtocol p) {
    if (p == TransferProtocol::Rsync || p == TransferProtocol::RemoteDirectRsync) {
        return QColor(56, 189, 248);  // Cyan
    }
    if (p == TransferProtocol::Sftp || p == TransferProtocol::RemoteRelaySftp) {
        return QColor(74, 222, 128);  // Green
    }
    return QColor(251, 191, 36);  // Amber
}

QColor colorForStatus(TransferStatus st) {
    switch (st) {
        case TransferStatus::Failed: return QColor(248, 113, 113);
        case TransferStatus::Completed: return QColor(74, 222, 128);
        case TransferStatus::Paused: return QColor(251, 191, 36);
        case TransferStatus::Canceled: return QColor(148, 163, 184);
        case TransferStatus::Scanning: return QColor(56, 189, 248);
        default: return QColor();
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// TransferQueueModel
// ---------------------------------------------------------------------------
TransferQueueModel::TransferQueueModel(TransferManager* manager, QObject* parent)
    : QAbstractItemModel(parent) {
    if (manager) {
        for (const TransferBatch& b : manager->batches()) {
            batches_.insert(b.id, b);
            batchChildren_.insert(b.id, {});
            topItems_.push_back({true, b.id});
        }
        for (const TransferTask& t : manager->tasks()) {
            tasks_.insert(t.id, t);
            if (t.batchId > 0 && batchChildren_.contains(t.batchId)) {
                batchChildren_[t.batchId].push_back(t.id);
            } else if (t.batchId == 0) {
                topItems_.push_back({false, t.id});
            }
        }
        connect(manager, &TransferManager::batchAdded, this, &TransferQueueModel::onBatchAdded);
        connect(manager, &TransferManager::batchUpdated, this, &TransferQueueModel::onBatchUpdated);
        connect(manager, &TransferManager::batchRemoved, this, &TransferQueueModel::onBatchRemoved);
        connect(manager, &TransferManager::taskAdded, this, &TransferQueueModel::onTaskAdded);
        connect(manager, &TransferManager::taskUpdated, this, &TransferQueueModel::onTaskUpdated);
        connect(manager, &TransferManager::taskRemoved, this, &TransferQueueModel::onTaskRemoved);
    }
}

int TransferQueueModel::findTopRow(bool isBatch, uint64_t id) const {
    for (size_t i = 0; i < topItems_.size(); ++i) {
        if (topItems_[i].isBatch == isBatch && topItems_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int TransferQueueModel::findChildRow(uint64_t batchId, uint64_t taskId) const {
    auto it = batchChildren_.constFind(batchId);
    if (it == batchChildren_.constEnd()) return -1;
    const auto& vec = it.value();
    for (size_t i = 0; i < vec.size(); ++i) {
        if (vec[i] == taskId) return static_cast<int>(i);
    }
    return -1;
}

QModelIndex TransferQueueModel::index(int row, int column, const QModelIndex& parent) const {
    if (row < 0 || column < 0 || column >= ColCount) return {};
    if (!parent.isValid()) {
        if (static_cast<size_t>(row) >= topItems_.size()) return {};
        return createIndex(row, column, static_cast<quintptr>(0));
    }
    if (parent.internalId() != 0) return {};  // Depth capped at 2
    if (parent.row() < 0 || static_cast<size_t>(parent.row()) >= topItems_.size()) return {};
    const TopEntry& te = topItems_[static_cast<size_t>(parent.row())];
    if (!te.isBatch) return {};
    const auto it = batchChildren_.constFind(te.id);
    if (it == batchChildren_.constEnd() || static_cast<size_t>(row) >= it.value().size()) return {};
    return createIndex(row, column, static_cast<quintptr>(te.id));
}

QModelIndex TransferQueueModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) return {};
    const quintptr bid = child.internalId();
    if (bid == 0) return {};
    const int topRow = findTopRow(true, static_cast<uint64_t>(bid));
    if (topRow < 0) return {};
    return createIndex(topRow, 0, static_cast<quintptr>(0));
}

int TransferQueueModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) return 0;
    if (!parent.isValid()) {
        return static_cast<int>(topItems_.size());
    }
    if (parent.internalId() != 0) return 0;
    if (parent.row() < 0 || static_cast<size_t>(parent.row()) >= topItems_.size()) return 0;
    const TopEntry& te = topItems_[static_cast<size_t>(parent.row())];
    if (!te.isBatch) return 0;
    return static_cast<int>(batchChildren_.value(te.id).size());
}

int TransferQueueModel::columnCount(const QModelIndex& /*parent*/) const {
    return ColCount;
}

bool TransferQueueModel::isBatchIndex(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.internalId() != 0) return false;
    if (idx.row() < 0 || static_cast<size_t>(idx.row()) >= topItems_.size()) return false;
    return topItems_[static_cast<size_t>(idx.row())].isBatch;
}

uint64_t TransferQueueModel::idForIndex(const QModelIndex& idx) const {
    if (!idx.isValid()) return 0;
    const quintptr bid = idx.internalId();
    if (bid == 0) {
        if (idx.row() < 0 || static_cast<size_t>(idx.row()) >= topItems_.size()) return 0;
        return topItems_[static_cast<size_t>(idx.row())].id;
    }
    const auto it = batchChildren_.constFind(static_cast<uint64_t>(bid));
    if (it == batchChildren_.constEnd() || idx.row() < 0 ||
        static_cast<size_t>(idx.row()) >= it.value().size()) {
        return 0;
    }
    return it.value()[static_cast<size_t>(idx.row())];
}

const TransferBatch* TransferQueueModel::batchForIndex(const QModelIndex& idx) const {
    if (!isBatchIndex(idx)) return nullptr;
    auto it = batches_.constFind(idForIndex(idx));
    return it != batches_.constEnd() ? &it.value() : nullptr;
}

const TransferTask* TransferQueueModel::taskForIndex(const QModelIndex& idx) const {
    if (!idx.isValid() || isBatchIndex(idx)) return nullptr;
    auto it = tasks_.constFind(idForIndex(idx));
    return it != tasks_.constEnd() ? &it.value() : nullptr;
}

QVariant TransferQueueModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};

    if (const TransferBatch* b = batchForIndex(index)) {
        if (role == RoleIsBatch) return true;
        if (role == RoleItemId) return QVariant::fromValue<qulonglong>(b->id);
        if (role == RoleProgressPercent) return b->progressPercent;
        if (role == RoleStatusInt) return static_cast<int>(b->status);
        if (role == RoleErrorText) return b->errorMessage;

        if (role == Qt::DecorationRole && index.column() == ColItem) {
            return IconCache::folderIcon();
        }
        if (role == Qt::ForegroundRole) {
            if (index.column() == ColProtocol) return colorForProtocol(b->activeProtocol);
            if (index.column() == ColFiles && b->failedFiles > 0) return QColor(248, 113, 113);
            if (index.column() == ColStatus) {
                QColor c = colorForStatus(b->status);
                if (c.isValid()) return c;
            }
        }
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
                case ColItem:
                    return b->itemName;
                case ColFiles:
                    if (b->isScanning) return QStringLiteral("Scanning...");
                    if (b->failedFiles > 0) {
                        return QStringLiteral("%1 / %2 (%3 err)")
                            .arg(b->completedFiles).arg(b->totalFiles).arg(b->failedFiles);
                    }
                    return QStringLiteral("%1 / %2").arg(b->completedFiles).arg(b->totalFiles);
                case ColRoute:
                    return QStringLiteral("%1 → %2").arg(b->sourceEndpoint.label(), b->destEndpoint.label());
                case ColProtocol:
                    return b->isScanning ? QStringLiteral("...") : protocolToString(b->activeProtocol);
                case ColProgress:
                    return QStringLiteral("%1%").arg(b->progressPercent);
                case ColSize:
                    if (b->totalBytes > 0) {
                        return QStringLiteral("%1 / %2")
                            .arg(formatBytes(b->transferredBytes), formatBytes(b->totalBytes));
                    }
                    return QStringLiteral("--");
                case ColSpeed:
                    return formatSpeed(b->speedBytesPerSec);
                case ColEta:
                    if (b->status != TransferStatus::Running || b->totalBytes <= b->transferredBytes) {
                        return QStringLiteral("--");
                    }
                    return formatEta(b->totalBytes - b->transferredBytes, b->speedBytesPerSec);
                case ColStatus:
                    return b->statusMessage.isEmpty() ? transferStatusToString(b->status) : b->statusMessage;
                default:
                    return {};
            }
        }
        if (role == Qt::ToolTipRole) {
            QString tip = QStringLiteral("Folder: %1\nFrom: %2:%3\nTo: %4:%5\nFiles: %6/%7 completed (%8 failed, %9 paused)")
                              .arg(b->itemName, b->sourceEndpoint.label(), b->sourcePath,
                                   b->destEndpoint.label(), b->destDir)
                              .arg(b->completedFiles).arg(b->totalFiles)
                              .arg(b->failedFiles).arg(b->pausedFiles);
            if (!b->errorMessage.isEmpty()) {
                tip += QStringLiteral("\nFirst error: %1").arg(b->errorMessage);
            }
            return tip;
        }
        return {};
    }

    if (const TransferTask* t = taskForIndex(index)) {
        if (role == RoleIsBatch) return false;
        if (role == RoleItemId) return QVariant::fromValue<qulonglong>(t->id);
        if (role == RoleProgressPercent) return t->progressPercent;
        if (role == RoleStatusInt) return static_cast<int>(t->status);
        if (role == RoleErrorText) return t->errorMessage;

        if (role == Qt::DecorationRole && index.column() == ColItem) {
            FileEntry tmp;
            tmp.name = t->itemName;
            return IconCache::iconForEntry(tmp);
        }
        if (role == Qt::ForegroundRole) {
            if (index.column() == ColProtocol) return colorForProtocol(t->activeProtocol);
            if (index.column() == ColStatus) {
                QColor c = colorForStatus(t->status);
                if (c.isValid()) return c;
            }
        }
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
                case ColItem:
                    return t->relativePath.isEmpty() ? t->itemName : t->relativePath;
                case ColFiles:
                    return t->status == TransferStatus::Failed ? QStringLiteral("1 (err)") : QStringLiteral("1");
                case ColRoute:
                    return QStringLiteral("%1 → %2").arg(t->sourceEndpoint.label(), t->destEndpoint.label());
                case ColProtocol:
                    return protocolToString(t->activeProtocol);
                case ColProgress:
                    return QStringLiteral("%1%").arg(t->progressPercent);
                case ColSize:
                    if (t->totalBytes > 0) {
                        return QStringLiteral("%1 / %2")
                            .arg(formatBytes(t->transferredBytes), formatBytes(t->totalBytes));
                    }
                    return t->transferredBytes > 0 ? formatBytes(t->transferredBytes) : QStringLiteral("0 B");
                case ColSpeed:
                    return formatSpeed(t->speedBytesPerSec);
                case ColEta:
                    if (t->status != TransferStatus::Running || t->totalBytes <= t->transferredBytes) {
                        return QStringLiteral("--");
                    }
                    return formatEta(t->totalBytes - t->transferredBytes, t->speedBytesPerSec);
                case ColStatus:
                    if (t->status == TransferStatus::Failed && !t->errorMessage.isEmpty()) {
                        return QStringLiteral("Error: %1").arg(t->errorMessage);
                    }
                    return t->statusMessage.isEmpty() ? transferStatusToString(t->status) : t->statusMessage;
                default:
                    return {};
            }
        }
        if (role == Qt::ToolTipRole) {
            QString tip = QStringLiteral("File: %1\nFrom: %2:%3\nTo: %4:%5\nProtocol: %6\nStatus: %7")
                              .arg(t->itemName, t->sourceEndpoint.label(), t->sourcePath,
                                   t->destEndpoint.label(), t->destDir,
                                   protocolToString(t->activeProtocol),
                                   transferStatusToString(t->status));
            if (!t->errorMessage.isEmpty()) {
                tip += QStringLiteral("\nError Details: %1").arg(t->errorMessage);
            }
            return tip;
        }
    }

    return {};
}

QVariant TransferQueueModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case ColItem: return QStringLiteral("Item / File");
        case ColFiles: return QStringLiteral("Files");
        case ColRoute: return QStringLiteral("Route");
        case ColProtocol: return QStringLiteral("Protocol");
        case ColProgress: return QStringLiteral("Progress");
        case ColSize: return QStringLiteral("Size");
        case ColSpeed: return QStringLiteral("Speed");
        case ColEta: return QStringLiteral("ETA");
        case ColStatus: return QStringLiteral("Status / Error");
        default: return {};
    }
}

void TransferQueueModel::onBatchAdded(const TransferBatch& b) {
    const int row = static_cast<int>(topItems_.size());
    beginInsertRows(QModelIndex(), row, row);
    batches_.insert(b.id, b);
    batchChildren_.insert(b.id, {});
    topItems_.push_back({true, b.id});
    endInsertRows();
}

void TransferQueueModel::onBatchUpdated(const TransferBatch& b) {
    const int row = findTopRow(true, b.id);
    if (row < 0) return;
    batches_[b.id] = b;
    emit dataChanged(index(row, 0), index(row, ColCount - 1));
}

void TransferQueueModel::onBatchRemoved(uint64_t batchId) {
    const int row = findTopRow(true, batchId);
    if (row < 0) return;
    beginRemoveRows(QModelIndex(), row, row);
    for (uint64_t cid : batchChildren_.value(batchId)) {
        tasks_.remove(cid);
    }
    batchChildren_.remove(batchId);
    batches_.remove(batchId);
    topItems_.erase(topItems_.begin() + row);
    endRemoveRows();
}

void TransferQueueModel::onTaskAdded(const TransferTask& t) {
    if (t.batchId > 0) {
        const int parentRow = findTopRow(true, t.batchId);
        if (parentRow < 0) return;
        const QModelIndex parentIdx = index(parentRow, 0);
        auto& children = batchChildren_[t.batchId];
        const int row = static_cast<int>(children.size());
        beginInsertRows(parentIdx, row, row);
        tasks_.insert(t.id, t);
        children.push_back(t.id);
        endInsertRows();
        return;
    }

    const int row = static_cast<int>(topItems_.size());
    beginInsertRows(QModelIndex(), row, row);
    tasks_.insert(t.id, t);
    topItems_.push_back({false, t.id});
    endInsertRows();
}

void TransferQueueModel::onTaskUpdated(const TransferTask& t) {
    if (!tasks_.contains(t.id)) return;
    tasks_[t.id] = t;
    if (t.batchId > 0) {
        const int parentRow = findTopRow(true, t.batchId);
        const int childRow = findChildRow(t.batchId, t.id);
        if (parentRow >= 0 && childRow >= 0) {
            const QModelIndex pIdx = index(parentRow, 0);
            emit dataChanged(index(childRow, 0, pIdx), index(childRow, ColCount - 1, pIdx));
        }
    } else {
        const int row = findTopRow(false, t.id);
        if (row >= 0) {
            emit dataChanged(index(row, 0), index(row, ColCount - 1));
        }
    }
}

void TransferQueueModel::onTaskRemoved(uint64_t taskId) {
    auto it = tasks_.constFind(taskId);
    if (it == tasks_.constEnd()) return;
    const uint64_t bid = it.value().batchId;

    if (bid > 0) {
        const int parentRow = findTopRow(true, bid);
        const int childRow = findChildRow(bid, taskId);
        if (parentRow >= 0 && childRow >= 0) {
            const QModelIndex pIdx = index(parentRow, 0);
            beginRemoveRows(pIdx, childRow, childRow);
            auto& vec = batchChildren_[bid];
            vec.erase(vec.begin() + childRow);
            tasks_.remove(taskId);
            endRemoveRows();
        } else {
            tasks_.remove(taskId);
        }
        return;
    }

    const int row = findTopRow(false, taskId);
    if (row >= 0) {
        beginRemoveRows(QModelIndex(), row, row);
        tasks_.remove(taskId);
        topItems_.erase(topItems_.begin() + row);
        endRemoveRows();
    } else {
        tasks_.remove(taskId);
    }
}

// ---------------------------------------------------------------------------
// TransferQueueFilterProxy
// ---------------------------------------------------------------------------
TransferQueueFilterProxy::TransferQueueFilterProxy(QObject* parent)
    : QSortFilterProxyModel(parent) {
    setRecursiveFilteringEnabled(true);
    setDynamicSortFilter(true);
}

void TransferQueueFilterProxy::setFilterMode(FilterMode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    invalidateFilter();
}

bool TransferQueueFilterProxy::matchesStatus(TransferStatus st) const {
    switch (mode_) {
        case FilterAll: return true;
        case FilterActive:
            return st == TransferStatus::Queued || st == TransferStatus::Negotiating ||
                   st == TransferStatus::Running || st == TransferStatus::Scanning;
        case FilterPaused:
            return st == TransferStatus::Paused;
        case FilterFailed:
            return st == TransferStatus::Failed;
    }
    return true;
}

bool TransferQueueFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    if (mode_ == FilterAll) return true;
    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!idx.isValid()) return false;
    const auto st = static_cast<TransferStatus>(idx.data(TransferQueueModel::RoleStatusInt).toInt());
    return matchesStatus(st);
}

// ---------------------------------------------------------------------------
// ProgressBarDelegate
// ---------------------------------------------------------------------------
void ProgressBarDelegate::paint(QPainter* painter,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
    const int pct = qBound(0, index.data(TransferQueueModel::RoleProgressPercent).toInt(), 100);
    const auto st = static_cast<TransferStatus>(index.data(TransferQueueModel::RoleStatusInt).toInt());

    painter->save();
    if (option.state & QStyle::State_Selected) {
        painter->fillRect(option.rect, option.palette.highlight());
    }
    QRect barRect = option.rect.adjusted(4, 4, -4, -4);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QColor(51, 65, 85));
    painter->setBrush(QColor(30, 41, 59));
    painter->drawRoundedRect(barRect, 3, 3);

    if (pct > 0) {
        QRect fill = barRect.adjusted(1, 1, -1, -1);
        fill.setWidth(std::max(2, (fill.width() * pct) / 100));
        QColor barColor(37, 99, 235);  // Blue running
        if (st == TransferStatus::Completed || pct >= 100) barColor = QColor(34, 197, 94);  // Green
        else if (st == TransferStatus::Failed) barColor = QColor(239, 68, 68);             // Red
        else if (st == TransferStatus::Paused) barColor = QColor(217, 119, 6);             // Amber
        painter->setPen(Qt::NoPen);
        painter->setBrush(barColor);
        painter->drawRoundedRect(fill, 2, 2);
    }

    painter->setPen(Qt::white);
    QString label = (st == TransferStatus::Scanning)
                        ? QStringLiteral("Scan")
                        : (st == TransferStatus::Paused ? QStringLiteral("%1% (Paused)").arg(pct)
                                                        : QStringLiteral("%1%").arg(pct));
    painter->drawText(barRect, Qt::AlignCenter, label);
    painter->restore();
}

// ---------------------------------------------------------------------------
// TransferQueueWidget
// ---------------------------------------------------------------------------
TransferQueueWidget::TransferQueueWidget(TransferManager* manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    // Granular Xftp-style control bar
    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(6, 4, 6, 2);
    bar->setSpacing(5);

    auto* pauseSelBtn = new QPushButton(QStringLiteral("Pause"), this);
    pauseSelBtn->setToolTip(QStringLiteral("Pause selected files or folder"));
    connect(pauseSelBtn, &QPushButton::clicked, this, &TransferQueueWidget::pauseSelected);

    auto* resumeSelBtn = new QPushButton(QStringLiteral("Resume"), this);
    resumeSelBtn->setToolTip(QStringLiteral("Resume selected paused files or folder"));
    connect(resumeSelBtn, &QPushButton::clicked, this, &TransferQueueWidget::resumeSelected);

    auto* removeSelBtn = new QPushButton(QStringLiteral("Delete"), this);
    removeSelBtn->setToolTip(QStringLiteral("Cancel & remove selected files or folder from queue (Del)"));
    connect(removeSelBtn, &QPushButton::clicked, this, &TransferQueueWidget::removeSelected);

    auto* retryBtn = new QPushButton(QStringLiteral("Retry Failed"), this);
    retryBtn->setToolTip(QStringLiteral("Retry selected failed items (or all failed items)"));
    connect(retryBtn, &QPushButton::clicked, this, &TransferQueueWidget::retrySelectedOrFailed);

    auto* clearBtn = new QPushButton(QStringLiteral("Clear Done"), this);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        if (manager_) manager_->clearFinished();
    });

    pauseAllBtn_ = new QPushButton(QStringLiteral("Pause All"), this);
    connect(pauseAllBtn_, &QPushButton::clicked, this, [this]() {
        if (!manager_) return;
        const bool next = !manager_->isPaused();
        manager_->setPaused(next);
        pauseAllBtn_->setText(next ? QStringLiteral("Resume All") : QStringLiteral("Pause All"));
    });

    filterCombo_ = new QComboBox(this);
    filterCombo_->addItem(QStringLiteral("Show: All"), static_cast<int>(TransferQueueFilterProxy::FilterAll));
    filterCombo_->addItem(QStringLiteral("Show: Active"), static_cast<int>(TransferQueueFilterProxy::FilterActive));
    filterCombo_->addItem(QStringLiteral("Show: Paused"), static_cast<int>(TransferQueueFilterProxy::FilterPaused));
    filterCombo_->addItem(QStringLiteral("Show: Failed Only"), static_cast<int>(TransferQueueFilterProxy::FilterFailed));

    concurSpin_ = new QSpinBox(this);
    concurSpin_->setRange(1, 8);
    concurSpin_->setValue(manager_ ? manager_->maxConcurrent() : 3);
    concurSpin_->setPrefix(QStringLiteral("Workers: "));
    connect(concurSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int n) {
        if (manager_) manager_->setMaxConcurrent(n);
    });

    bar->addWidget(pauseSelBtn);
    bar->addWidget(resumeSelBtn);
    bar->addWidget(removeSelBtn);
    bar->addWidget(retryBtn);
    bar->addWidget(clearBtn);
    bar->addWidget(pauseAllBtn_);
    bar->addStretch(1);
    bar->addWidget(filterCombo_);
    bar->addWidget(concurSpin_);
    root->addLayout(bar);

    model_ = new TransferQueueModel(manager_, this);
    proxy_ = new TransferQueueFilterProxy(this);
    proxy_->setSourceModel(model_);

    connect(filterCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        proxy_->setFilterMode(
            static_cast<TransferQueueFilterProxy::FilterMode>(filterCombo_->currentData().toInt()));
        if (filterCombo_->currentData().toInt() == static_cast<int>(TransferQueueFilterProxy::FilterFailed)) {
            view_->expandAll();
        }
    });

    view_ = new QTreeView(this);
    view_->setModel(proxy_);
    view_->setItemDelegateForColumn(TransferQueueModel::ColProgress, new ProgressBarDelegate(view_));
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setAlternatingRowColors(true);
    view_->setUniformRowHeights(true);
    view_->setRootIsDecorated(true);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->header()->setStretchLastSection(true);
    view_->setColumnWidth(TransferQueueModel::ColItem, 230);
    view_->setColumnWidth(TransferQueueModel::ColFiles, 92);
    view_->setColumnWidth(TransferQueueModel::ColRoute, 155);
    view_->setColumnWidth(TransferQueueModel::ColProtocol, 96);
    view_->setColumnWidth(TransferQueueModel::ColProgress, 118);
    view_->setColumnWidth(TransferQueueModel::ColSize, 124);
    view_->setColumnWidth(TransferQueueModel::ColSpeed, 86);
    view_->setColumnWidth(TransferQueueModel::ColEta, 62);

    connect(view_, &QWidget::customContextMenuRequested, this, &TransferQueueWidget::showContextMenu);
    connect(view_, &QTreeView::doubleClicked, this, &TransferQueueWidget::showErrorDetails);

    auto* delSc = new QShortcut(QKeySequence::Delete, view_);
    delSc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(delSc, &QShortcut::activated, this, &TransferQueueWidget::removeSelected);

    root->addWidget(view_, 1);
}

TransferQueueWidget::SelectionSet TransferQueueWidget::collectSelection() const {
    SelectionSet sel;
    if (!view_ || !view_->selectionModel()) return sel;

    QSet<uint64_t> seenBatches;
    QSet<uint64_t> seenTasks;
    for (const QModelIndex& proxyIdx : view_->selectionModel()->selectedRows()) {
        const QModelIndex srcIdx = proxy_->mapToSource(proxyIdx);
        if (!srcIdx.isValid()) continue;
        if (model_->isBatchIndex(srcIdx)) {
            uint64_t bid = model_->idForIndex(srcIdx);
            if (!seenBatches.contains(bid)) {
                seenBatches.insert(bid);
                sel.batchIds.push_back(bid);
            }
        } else {
            uint64_t tid = model_->idForIndex(srcIdx);
            if (!seenTasks.contains(tid)) {
                seenTasks.insert(tid);
                sel.taskIds.push_back(tid);
            }
        }
    }
    return sel;
}

void TransferQueueWidget::pauseSelected() {
    if (!manager_) return;
    const auto sel = collectSelection();
    for (uint64_t bid : sel.batchIds) manager_->pauseBatch(bid);
    for (uint64_t tid : sel.taskIds) manager_->pauseTask(tid);
}

void TransferQueueWidget::resumeSelected() {
    if (!manager_) return;
    const auto sel = collectSelection();
    for (uint64_t bid : sel.batchIds) manager_->resumeBatch(bid);
    for (uint64_t tid : sel.taskIds) manager_->resumeTask(tid);
}

void TransferQueueWidget::cancelSelected() {
    if (!manager_) return;
    const auto sel = collectSelection();
    for (uint64_t bid : sel.batchIds) manager_->cancelBatch(bid);
    for (uint64_t tid : sel.taskIds) manager_->cancelTask(tid);
}

void TransferQueueWidget::removeSelected() {
    if (!manager_) return;
    const auto sel = collectSelection();
    // Remove individual tasks first, then batches (safe even if both parent & child selected)
    for (uint64_t tid : sel.taskIds) manager_->removeTask(tid);
    for (uint64_t bid : sel.batchIds) manager_->removeBatch(bid);
}

void TransferQueueWidget::retrySelectedOrFailed() {
    if (!manager_) return;
    const auto sel = collectSelection();
    bool retriedAny = false;
    for (uint64_t bid : sel.batchIds) {
        manager_->retryFailedInBatch(bid);
        retriedAny = true;
    }
    for (uint64_t tid : sel.taskIds) {
        if (const TransferTask* t = manager_->findTask(tid)) {
            if (t->status == TransferStatus::Failed || t->status == TransferStatus::Canceled) {
                manager_->retryTask(tid);
                retriedAny = true;
            }
        }
    }
    if (!retriedAny) {
        manager_->retryAllFailed();
    }
}

void TransferQueueWidget::showErrorDetails(const QModelIndex& proxyIdx) {
    if (!proxyIdx.isValid()) return;
    const QModelIndex srcIdx = proxy_->mapToSource(proxyIdx);
    if (!srcIdx.isValid()) return;

    if (const TransferTask* t = model_->taskForIndex(srcIdx)) {
        if (t->status != TransferStatus::Failed && t->errorMessage.isEmpty()) return;
        const QString detail = QStringLiteral(
                                   "File: %1\n"
                                   "Source: %2:%3\n"
                                   "Destination: %4:%5\n"
                                   "Protocol: %6\n\n"
                                   "Error:\n%7")
                                   .arg(t->relativePath.isEmpty() ? t->itemName : t->relativePath,
                                        t->sourceEndpoint.label(), t->sourcePath,
                                        t->destEndpoint.label(), t->destDir,
                                        protocolToString(t->activeProtocol),
                                        t->errorMessage.isEmpty() ? t->statusMessage : t->errorMessage);
        QMessageBox box(QMessageBox::Warning, QStringLiteral("File Transfer Error"), detail,
                        QMessageBox::Ok, this);
        QPushButton* copyBtn = box.addButton(QStringLiteral("Copy Error"), QMessageBox::ActionRole);
        box.exec();
        if (box.clickedButton() == copyBtn) {
            QApplication::clipboard()->setText(detail);
        }
    } else if (const TransferBatch* b = model_->batchForIndex(srcIdx)) {
        if (b->failedFiles > 0) {
            view_->expand(proxyIdx);
        } else if (!b->errorMessage.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Directory Transfer Error"), b->errorMessage);
        }
    }
}

void TransferQueueWidget::showContextMenu(const QPoint& pos) {
    QMenu menu(this);
    const auto sel = collectSelection();
    const bool hasSel = !sel.batchIds.empty() || !sel.taskIds.empty();

    if (hasSel) {
        menu.addAction(QStringLiteral("Pause Selected"), this, &TransferQueueWidget::pauseSelected);
        menu.addAction(QStringLiteral("Resume Selected"), this, &TransferQueueWidget::resumeSelected);
        menu.addAction(QStringLiteral("Retry Failed Selected"), this, &TransferQueueWidget::retrySelectedOrFailed);
        menu.addAction(QStringLiteral("Cancel Selected"), this, &TransferQueueWidget::cancelSelected);
        menu.addSeparator();
        menu.addAction(QStringLiteral("Remove / Delete from Queue\tDel"), this, &TransferQueueWidget::removeSelected);

        const QModelIndex idx = view_->indexAt(pos);
        if (idx.isValid()) {
            const QString errTxt = idx.data(TransferQueueModel::RoleErrorText).toString();
            if (!errTxt.isEmpty()) {
                menu.addSeparator();
                menu.addAction(QStringLiteral("View Error Details..."), this, [this, idx]() {
                    showErrorDetails(idx);
                });
                menu.addAction(QStringLiteral("Copy Error Message"), this, [errTxt]() {
                    QApplication::clipboard()->setText(errTxt);
                });
            }
        }
        menu.addSeparator();
    }

    menu.addAction(QStringLiteral("Expand All Folders"), view_, &QTreeView::expandAll);
    menu.addAction(QStringLiteral("Collapse All Folders"), view_, &QTreeView::collapseAll);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Retry All Failed"), this, [this]() {
        if (manager_) manager_->retryAllFailed();
    });
    menu.addAction(QStringLiteral("Clear Completed"), this, [this]() {
        if (manager_) manager_->clearFinished();
    });

    menu.exec(view_->viewport()->mapToGlobal(pos));
}

}  // namespace xsync
