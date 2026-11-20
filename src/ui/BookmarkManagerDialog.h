#pragma once

#include "core/Types.h"

#include <QDialog>
#include <vector>

class QLineEdit;
class QPushButton;
class QTableWidget;
class QToolButton;

namespace xsync {

class SessionStore;

// Modal sub-dialog for adding or editing a single bookmark entry.
class BookmarkEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit BookmarkEditDialog(const QString& title,
                                const QString& initialName,
                                const QString& initialPath,
                                QWidget* parent = nullptr);

    QString bookmarkName() const;
    QString bookmarkPath() const;

private:
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
};

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
    int selectedRow() const;
    void updateButtonStates();
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
    QPushButton* addCurBtn_ = nullptr;
    QPushButton* editBtn_ = nullptr;
    QPushButton* delBtn_ = nullptr;
    QToolButton* upBtn_ = nullptr;
    QToolButton* downBtn_ = nullptr;
    QPushButton* openBtn_ = nullptr;
    QString chosenPath_;
};

}  // namespace xsync
