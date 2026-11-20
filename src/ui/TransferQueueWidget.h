#pragma once

#include "core/Types.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QWidget>
#include <vector>

class QComboBox;
class QPushButton;
class QSpinBox;
class QTreeView;

namespace xsync {

class TransferManager;

// Pure 2-level hierarchical Model/View for the Transfer Queue:
//   - Top-level rows: Directory TransferBatch groups AND standalone single-file TransferTasks
//   - Child rows under a TransferBatch: each individual file inside that directory
// Zero per-row QProgressBar child widgets -> flat memory usage even with thousands of files.
class TransferQueueModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column {
        ColItem = 0,
        ColFiles = 1,
        ColRoute = 2,
        ColProtocol = 3,
        ColProgress = 4,
        ColSize = 5,
        ColSpeed = 6,
        ColEta = 7,
        ColStatus = 8,
        ColCount = 9
    };

    enum CustomRole {
        RoleProgressPercent = Qt::UserRole + 1,
        RoleStatusInt = Qt::UserRole + 2,
        RoleIsBatch = Qt::UserRole + 3,
        RoleItemId = Qt::UserRole + 4,
        RoleErrorText = Qt::UserRole + 5
    };

    explicit TransferQueueModel(TransferManager* manager, QObject* parent = nullptr);

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    bool isBatchIndex(const QModelIndex& idx) const;
    uint64_t idForIndex(const QModelIndex& idx) const;
    const TransferBatch* batchForIndex(const QModelIndex& idx) const;
    const TransferTask* taskForIndex(const QModelIndex& idx) const;

private:
    struct TopEntry {
        bool isBatch = false;
        uint64_t id = 0;
    };

    int findTopRow(bool isBatch, uint64_t id) const;
    int findChildRow(uint64_t batchId, uint64_t taskId) const;

    void onBatchAdded(const TransferBatch& b);
    void onBatchUpdated(const TransferBatch& b);
    void onBatchRemoved(uint64_t batchId);
    void onTaskAdded(const TransferTask& t);
    void onTaskUpdated(const TransferTask& t);
    void onTaskRemoved(uint64_t taskId);

    std::vector<TopEntry> topItems_;
    QHash<uint64_t, TransferBatch> batches_;
    QHash<uint64_t, TransferTask> tasks_;
    QHash<uint64_t, std::vector<uint64_t>> batchChildren_;
};

class TransferQueueFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    enum FilterMode {
        FilterAll = 0,
        FilterActive = 1,
        FilterPaused = 2,
        FilterFailed = 3
    };

    explicit TransferQueueFilterProxy(QObject* parent = nullptr);
    void setFilterMode(FilterMode mode);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    bool matchesStatus(TransferStatus st) const;
    FilterMode mode_ = FilterAll;
};

class ProgressBarDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
};

class TransferQueueWidget : public QWidget {
    Q_OBJECT
public:
    explicit TransferQueueWidget(TransferManager* manager, QWidget* parent = nullptr);

protected:
    void changeEvent(QEvent* event) override;

private:
    struct SelectionSet {
        std::vector<uint64_t> batchIds;
        std::vector<uint64_t> taskIds;
    };

    SelectionSet collectSelection() const;
    void updateButtonStates();
    void pauseSelected();
    void resumeSelected();
    void cancelSelected();
    void removeSelected();
    void retrySelectedOrFailed();
    void showErrorDetails(const QModelIndex& proxyIdx);
    void showContextMenu(const QPoint& pos);

    TransferManager* manager_ = nullptr;
    TransferQueueModel* model_ = nullptr;
    TransferQueueFilterProxy* proxy_ = nullptr;
    QTreeView* view_ = nullptr;
    QPushButton* pauseSelBtn_ = nullptr;
    QPushButton* resumeSelBtn_ = nullptr;
    QPushButton* removeSelBtn_ = nullptr;
    QPushButton* pauseAllBtn_ = nullptr;
    QComboBox* filterCombo_ = nullptr;
    QSpinBox* concurSpin_ = nullptr;
};

}  // namespace xsync
