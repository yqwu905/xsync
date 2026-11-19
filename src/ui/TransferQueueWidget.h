#pragma once

#include "core/Types.h"

#include <QAbstractTableModel>
#include <QStyledItemDelegate>
#include <QWidget>
#include <vector>

class QPushButton;
class QSpinBox;
class QTableView;

namespace xsync {

class TransferManager;

// Pure model-view transfer queue (NO QProgressBar per row -> zero widget leaks!).
class TransferQueueModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColId = 0,
        ColName = 1,
        ColDirection = 2,
        ColProtocol = 3,
        ColProgress = 4,
        ColSize = 5,
        ColSpeed = 6,
        ColEta = 7,
        ColStatus = 8,
        ColCount = 9
    };

    explicit TransferQueueModel(TransferManager* manager, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    uint64_t taskIdAt(int row) const;
    const TransferTask* taskAt(int row) const;

private:
    int findRow(uint64_t taskId) const;
    void onTaskAdded(const TransferTask& t);
    void onTaskUpdated(const TransferTask& t);
    void onTaskRemoved(uint64_t taskId);

    std::vector<TransferTask> rows_;
};

class ProgressBarDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
};

class TransferQueueWidget : public QWidget {
    Q_OBJECT
public:
    explicit TransferQueueWidget(TransferManager* manager, QWidget* parent = nullptr);

private:
    void cancelSelected();
    void retrySelected();

    TransferManager* manager_ = nullptr;
    TransferQueueModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    QPushButton* pauseBtn_ = nullptr;
    QSpinBox* concurSpin_ = nullptr;
};

}  // namespace xsync
