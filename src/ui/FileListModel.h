#pragma once

#include "core/Types.h"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <vector>

namespace xsync {

// Virtual flat-vector file model. Zero QStandardItem allocations; shrinks heap capacity
// automatically after leaving large directories to prevent memory retention.
class FileListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName = 0,
        ColSize = 1,
        ColModified = 2,
        ColPermissions = 3,
        ColType = 4,
        ColCount = 5
    };

    explicit FileListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void setEntries(std::vector<FileEntry> entries, bool includeParentLink, const QString& parentPath);
    void clear();

    const FileEntry* entryAt(int row) const;
    size_t entryCount() const { return entries_.size(); }
    size_t vectorCapacity() const { return entries_.capacity(); }

private:
    std::vector<FileEntry> entries_;
};

class FileSortFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit FileSortFilterProxyModel(QObject* parent = nullptr);

    void setShowHidden(bool show);
    bool showHidden() const { return showHidden_; }

    void setNameFilter(const QString& text);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool lessThan(const QModelIndex& sourceLeft, const QModelIndex& sourceRight) const override;

private:
    bool showHidden_ = true;
    QString nameFilter_;
};

}  // namespace xsync
