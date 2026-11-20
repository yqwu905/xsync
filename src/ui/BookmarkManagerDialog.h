#pragma once

#include "core/Types.h"

#include <QDialog>
#include <vector>

class QTableWidget;

namespace xsync {

class SessionStore;

// Dialog to manage per-host saved path bookmarks (Add / Edit / Reorder / Delete / Open).
class BookmarkManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit BookmarkManagerDialog(SessionStore* store,
                                   const EndpointSpec& endpoint,
                                   const QString& currentPath,
                                   QWidget* parent = nullptr);

    QString chosenPath() const { return chosenPath_; }

private:
    void setupUi();
    void reloadTable();
    void persistTable();
    void onAdd(const QString& presetPath = {});
    void onEdit();
    void onDelete();
    void onMove(int delta);
    void onOpenSelected();

    SessionStore* store_ = nullptr;
    EndpointSpec endpoint_;
    QString currentPath_;
    std::vector<PathBookmark> items_;
    QTableWidget* table_ = nullptr;
    QString chosenPath_;
};

}  // namespace xsync
