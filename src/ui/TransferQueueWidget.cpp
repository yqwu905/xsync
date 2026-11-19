#include "ui/TransferQueueWidget.h"

#include "core/TransferManager.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QStyleOptionProgressBar>
#include <QTableView>
#include <QVBoxLayout>

namespace xsync {

TransferQueueModel::TransferQueueModel(TransferManager* manager, QObject* parent)
    : QAbstractTableModel(parent) {
    if (manager) {
        rows_ = manager->tasks();
        connect(manager, &TransferManager::taskAdded, this, &TransferQueueModel::onTaskAdded);
        connect(manager, &TransferManager::taskUpdated, this, &TransferQueueModel::onTaskUpdated);
        connect(manager, &TransferManager::taskRemoved, this, &TransferQueueModel::onTaskRemoved);
    }
}

int TransferQueueModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(rows_.size());
}

int TransferQueueModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ColCount;
}

QVariant TransferQueueModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<size_t>(index.row()) >= rows_.size()) {
        return {};
    }
    const TransferTask& t = rows_[static_cast<size_t>(index.row())];

    if (role == Qt::UserRole && index.column() == ColProgress) {
        return t.progressPercent;
    }

    if (role == Qt::ForegroundRole) {
        if (index.column() == ColProtocol) {
            if (t.activeProtocol == TransferProtocol::Rsync ||
                t.activeProtocol == TransferProtocol::RemoteDirectRsync) {
                return QColor(56, 189, 248);  // Cyan for rsync
            }
            if (t.activeProtocol == TransferProtocol::Sftp ||
                t.activeProtocol == TransferProtocol::RemoteRelaySftp) {
                return QColor(74, 222, 128);  // Green for SFTP
            }
            return QColor(251, 191, 36);  // Amber for SCP / Local
        }
        if (index.column() == ColStatus) {
            if (t.status == TransferStatus::Failed) return QColor(248, 113, 113);
            if (t.status == TransferStatus::Completed) return QColor(74, 222, 128);
            if (t.status == TransferStatus::Canceled) return QColor(148, 163, 184);
        }
    }

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case ColId: return QStringLiteral("#%1").arg(t.id);
            case ColName: return t.itemName;
            case ColDirection:
                return QStringLiteral("%1 → %2")
                    .arg(t.sourceEndpoint.label(), t.destEndpoint.label());
            case ColProtocol: return protocolToString(t.activeProtocol);
            case ColProgress: return QStringLiteral("%1%").arg(t.progressPercent);
            case ColSize:
                if (t.totalBytes > 0) {
                    return QStringLiteral("%1 / %2")
                        .arg(formatBytes(t.transferredBytes), formatBytes(t.totalBytes));
                }
                return t.transferredBytes > 0 ? formatBytes(t.transferredBytes) : QStringLiteral("--");
            case ColSpeed: return formatSpeed(t.speedBytesPerSec);
            case ColEta: {
                if (t.status != TransferStatus::Running || t.totalBytes <= t.transferredBytes) {
                    return QStringLiteral("--");
                }
                return formatEta(t.totalBytes - t.transferredBytes, t.speedBytesPerSec);
            }
            case ColStatus: return t.statusMessage.isEmpty() ? transferStatusToString(t.status) : t.statusMessage;
            default: return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        return QStringLiteral("From: %1:%2\nTo: %3:%4\nProtocol: %5\nStatus: %6")
            .arg(t.sourceEndpoint.label(), t.sourcePath,
                 t.destEndpoint.label(), t.destDir,
                 protocolToString(t.activeProtocol), t.statusMessage);
    }
    return {};
}

QVariant TransferQueueModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case ColId: return QStringLiteral("ID");
        case ColName: return QStringLiteral("Item");
        case ColDirection: return QStringLiteral("Route");
        case ColProtocol: return QStringLiteral("Protocol");
        case ColProgress: return QStringLiteral("Progress");
        case ColSize: return QStringLiteral("Size");
        case ColSpeed: return QStringLiteral("Speed");
        case ColEta: return QStringLiteral("ETA");
        case ColStatus: return QStringLiteral("Status");
        default: return {};
    }
}

uint64_t TransferQueueModel::taskIdAt(int row) const {
    if (row < 0 || static_cast<size_t>(row) >= rows_.size()) return 0;
    return rows_[static_cast<size_t>(row)].id;
}

const TransferTask* TransferQueueModel::taskAt(int row) const {
    if (row < 0 || static_cast<size_t>(row) >= rows_.size()) return nullptr;
    return &rows_[static_cast<size_t>(row)];
}

int TransferQueueModel::findRow(uint64_t taskId) const {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].id == taskId) return static_cast<int>(i);
    }
    return -1;
}

void TransferQueueModel::onTaskAdded(const TransferTask& t) {
    const int row = static_cast<int>(rows_.size());
    beginInsertRows(QModelIndex(), row, row);
    rows_.push_back(t);
    endInsertRows();
}

void TransferQueueModel::onTaskUpdated(const TransferTask& t) {
    const int row = findRow(t.id);
    if (row < 0) return;
    rows_[static_cast<size_t>(row)] = t;
    emit dataChanged(index(row, 0), index(row, ColCount - 1));
}

void TransferQueueModel::onTaskRemoved(uint64_t taskId) {
    const int row = findRow(taskId);
    if (row < 0) return;
    beginRemoveRows(QModelIndex(), row, row);
    rows_.erase(rows_.begin() + row);
    endRemoveRows();
}

void ProgressBarDelegate::paint(QPainter* painter,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
    const int pct = qBound(0, index.data(Qt::UserRole).toInt(), 100);
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
        painter->setPen(Qt::NoPen);
        painter->setBrush(pct >= 100 ? QColor(34, 197, 94) : QColor(37, 99, 235));
        painter->drawRoundedRect(fill, 2, 2);
    }
    painter->setPen(Qt::white);
    painter->drawText(barRect, Qt::AlignCenter, QStringLiteral("%1%").arg(pct));
    painter->restore();
}

TransferQueueWidget::TransferQueueWidget(TransferManager* manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    // Compact controls bar
    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(6, 4, 6, 2);

    pauseBtn_ = new QPushButton(QStringLiteral("Pause Queue"), this);
    connect(pauseBtn_, &QPushButton::clicked, this, [this]() {
        if (!manager_) return;
        const bool next = !manager_->isPaused();
        manager_->setPaused(next);
        pauseBtn_->setText(next ? QStringLiteral("Resume Queue") : QStringLiteral("Pause Queue"));
    });

    auto* cancelBtn = new QPushButton(QStringLiteral("Cancel Selected"), this);
    connect(cancelBtn, &QPushButton::clicked, this, &TransferQueueWidget::cancelSelected);

    auto* retryBtn = new QPushButton(QStringLiteral("Retry Selected"), this);
    connect(retryBtn, &QPushButton::clicked, this, &TransferQueueWidget::retrySelected);

    auto* clearBtn = new QPushButton(QStringLiteral("Clear Completed"), this);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        if (manager_) manager_->clearFinished();
    });

    concurSpin_ = new QSpinBox(this);
    concurSpin_->setRange(1, 8);
    concurSpin_->setValue(manager_ ? manager_->maxConcurrent() : 3);
    concurSpin_->setPrefix(QStringLiteral("Workers: "));
    connect(concurSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int n) {
        if (manager_) manager_->setMaxConcurrent(n);
    });

    bar->addWidget(pauseBtn_);
    bar->addWidget(cancelBtn);
    bar->addWidget(retryBtn);
    bar->addWidget(clearBtn);
    bar->addStretch(1);
    bar->addWidget(concurSpin_);
    root->addLayout(bar);

    model_ = new TransferQueueModel(manager_, this);
    view_ = new QTableView(this);
    view_->setModel(model_);
    view_->setItemDelegateForColumn(TransferQueueModel::ColProgress, new ProgressBarDelegate(view_));
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setAlternatingRowColors(true);
    view_->setShowGrid(false);
    view_->verticalHeader()->setVisible(false);
    view_->verticalHeader()->setDefaultSectionSize(24);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->setColumnWidth(TransferQueueModel::ColId, 46);
    view_->setColumnWidth(TransferQueueModel::ColName, 190);
    view_->setColumnWidth(TransferQueueModel::ColDirection, 165);
    view_->setColumnWidth(TransferQueueModel::ColProtocol, 108);
    view_->setColumnWidth(TransferQueueModel::ColProgress, 110);
    view_->setColumnWidth(TransferQueueModel::ColSize, 130);
    view_->setColumnWidth(TransferQueueModel::ColSpeed, 90);
    view_->setColumnWidth(TransferQueueModel::ColEta, 65);
    root->addWidget(view_, 1);
}

void TransferQueueWidget::cancelSelected() {
    if (!manager_ || !view_->selectionModel()) return;
    for (const QModelIndex& idx : view_->selectionModel()->selectedRows()) {
        manager_->cancelTask(model_->taskIdAt(idx.row()));
    }
}

void TransferQueueWidget::retrySelected() {
    if (!manager_ || !view_->selectionModel()) return;
    for (const QModelIndex& idx : view_->selectionModel()->selectedRows()) {
        manager_->retryTask(model_->taskIdAt(idx.row()));
    }
}

}  // namespace xsync
