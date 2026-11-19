#include "ui/FileListModel.h"

#include "ui/IconCache.h"

#include <QDateTime>
#include <QFileInfo>

namespace xsync {

FileListModel::FileListModel(QObject* parent) : QAbstractTableModel(parent) {}

int FileListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(entries_.size());
}

int FileListModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ColCount;
}

QVariant FileListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<size_t>(index.row()) >= entries_.size()) {
        return {};
    }
    const FileEntry& e = entries_[static_cast<size_t>(index.row())];

    if (role == Qt::DecorationRole && index.column() == ColName) {
        return IconCache::iconForEntry(e);
    }

    if (role == Qt::TextAlignmentRole) {
        if (index.column() == ColSize) return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case ColName:
                return e.name;
            case ColSize:
                if (e.isParentLink || e.isDir) return QStringLiteral("<DIR>");
                return formatBytes(e.size);
            case ColModified:
                if (e.isParentLink || e.modifiedEpoch <= 0) return QString();
                return QDateTime::fromSecsSinceEpoch(e.modifiedEpoch)
                    .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            case ColPermissions:
                if (e.isParentLink) return QString();
                return formatPermissions(e.permissions, e.isDir, e.isSymlink);
            case ColType: {
                if (e.isParentLink) return QStringLiteral("Parent");
                if (e.isSymlink) return e.isDir ? QStringLiteral("Symlink (Dir)") : QStringLiteral("Symlink");
                if (e.isDir) return QStringLiteral("Folder");
                const int dot = e.name.lastIndexOf(QLatin1Char('.'));
                const QString ext = (dot > 0 && dot + 1 < e.name.size())
                                        ? e.name.mid(dot + 1).toUpper()
                                        : QString();
                return ext.isEmpty() ? QStringLiteral("File") : (ext + QStringLiteral(" File"));
            }
            default:
                return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        if (e.isParentLink) return QStringLiteral("Go to parent folder");
        if (e.isSymlink && !e.symlinkTarget.isEmpty()) {
            return QStringLiteral("%1 -> %2").arg(e.fullPath, e.symlinkTarget);
        }
        return e.fullPath;
    }

    return {};
}

QVariant FileListModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case ColName: return QStringLiteral("Name");
        case ColSize: return QStringLiteral("Size");
        case ColModified: return QStringLiteral("Modified");
        case ColPermissions: return QStringLiteral("Permissions");
        case ColType: return QStringLiteral("Type");
        default: return {};
    }
}

Qt::ItemFlags FileListModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::ItemIsDropEnabled;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    const FileEntry* e = entryAt(index.row());
    if (e && !e->isParentLink) {
        f |= Qt::ItemIsDragEnabled;
    }
    if (e && e->isDir) {
        f |= Qt::ItemIsDropEnabled;
    }
    return f;
}

void FileListModel::setEntries(std::vector<FileEntry> entries,
                               bool includeParentLink,
                               const QString& parentPath) {
    beginResetModel();
    const size_t prevCap = entries_.capacity();
    entries_.clear();

    const size_t total = entries.size() + (includeParentLink ? 1 : 0);
    // Shrink heap buffer after browsing huge folders (> 4096 items) to keep RAM flat
    if (prevCap > 4096 && total < prevCap / 4) {
        std::vector<FileEntry>().swap(entries_);
    }
    entries_.reserve(total);

    if (includeParentLink) {
        FileEntry up;
        up.name = QStringLiteral("..");
        up.fullPath = parentPath;
        up.isDir = true;
        up.isParentLink = true;
        entries_.push_back(std::move(up));
    }
    for (FileEntry& e : entries) {
        entries_.push_back(std::move(e));
    }
    endResetModel();
}

void FileListModel::clear() {
    beginResetModel();
    std::vector<FileEntry>().swap(entries_);
    endResetModel();
}

const FileEntry* FileListModel::entryAt(int row) const {
    if (row < 0 || static_cast<size_t>(row) >= entries_.size()) return nullptr;
    return &entries_[static_cast<size_t>(row)];
}

// ---------------------------------------------------------------------------
// FileSortFilterProxyModel
// ---------------------------------------------------------------------------
FileSortFilterProxyModel::FileSortFilterProxyModel(QObject* parent)
    : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

void FileSortFilterProxyModel::setShowHidden(bool show) {
    if (showHidden_ == show) return;
    showHidden_ = show;
    invalidateFilter();
}

void FileSortFilterProxyModel::setNameFilter(const QString& text) {
    const QString trimmed = text.trimmed();
    if (nameFilter_ == trimmed) return;
    nameFilter_ = trimmed;
    invalidateFilter();
}

bool FileSortFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& /*sourceParent*/) const {
    const auto* m = qobject_cast<const FileListModel*>(sourceModel());
    if (!m) return true;
    const FileEntry* e = m->entryAt(sourceRow);
    if (!e) return false;
    if (e->isParentLink) return true;
    if (!showHidden_ && e->name.startsWith(QLatin1Char('.'))) return false;
    if (!nameFilter_.isEmpty() && !e->name.contains(nameFilter_, Qt::CaseInsensitive)) return false;
    return true;
}

bool FileSortFilterProxyModel::lessThan(const QModelIndex& sourceLeft,
                                        const QModelIndex& sourceRight) const {
    const auto* m = qobject_cast<const FileListModel*>(sourceModel());
    if (!m) return QSortFilterProxyModel::lessThan(sourceLeft, sourceRight);
    const FileEntry* l = m->entryAt(sourceLeft.row());
    const FileEntry* r = m->entryAt(sourceRight.row());
    if (!l || !r) return false;

    const bool desc = (sortOrder() == Qt::DescendingOrder);

    // Always pin ".." to the very top regardless of sort order
    if (l->isParentLink != r->isParentLink) {
        return desc ? !l->isParentLink : l->isParentLink;
    }
    // Always keep directories grouped above regular files
    if (l->isDir != r->isDir) {
        return desc ? !l->isDir : l->isDir;
    }

    switch (sourceLeft.column()) {
        case FileListModel::ColName:
            return QString::compare(l->name, r->name, Qt::CaseInsensitive) < 0;
        case FileListModel::ColSize:
            if (l->size != r->size) return l->size < r->size;
            return QString::compare(l->name, r->name, Qt::CaseInsensitive) < 0;
        case FileListModel::ColModified:
            if (l->modifiedEpoch != r->modifiedEpoch) return l->modifiedEpoch < r->modifiedEpoch;
            return QString::compare(l->name, r->name, Qt::CaseInsensitive) < 0;
        case FileListModel::ColPermissions:
            if (l->permissions != r->permissions) return l->permissions < r->permissions;
            return QString::compare(l->name, r->name, Qt::CaseInsensitive) < 0;
        case FileListModel::ColType: {
            const QString extL = QFileInfo(l->name).suffix().toLower();
            const QString extR = QFileInfo(r->name).suffix().toLower();
            int cmp = QString::compare(extL, extR, Qt::CaseInsensitive);
            if (cmp != 0) return cmp < 0;
            return QString::compare(l->name, r->name, Qt::CaseInsensitive) < 0;
        }
        default:
            return QString::compare(l->name, r->name, Qt::CaseInsensitive) < 0;
    }
}

}  // namespace xsync
