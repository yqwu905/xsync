#pragma once

#include "core/Types.h"

#include <QDialog>
#include <optional>

class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace xsync {

class SessionStore;

class SessionManagerDialog : public QDialog {
    Q_OBJECT
public:
    enum class OpenTarget {
        ActivePane,
        LeftPane,
        RightPane,
        NewWindow
    };

    explicit SessionManagerDialog(SessionStore* store, QWidget* parent = nullptr);

signals:
    void openSessionRequested(const xsync::SessionProfile& profile, OpenTarget target);

private:
    void setupUi();
    void rebuildTree();
    std::optional<SessionProfile> selectedProfile() const;
    void onNewHost();
    void onEditHost();
    void onDuplicateHost();
    void onDeleteHost();
    void onImportSshConfig();
    void openSelected(OpenTarget target);

    SessionStore* store_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPushButton* openLeftBtn_ = nullptr;
    QPushButton* openRightBtn_ = nullptr;
    QPushButton* openWinBtn_ = nullptr;
    QPushButton* editBtn_ = nullptr;
    QPushButton* dupBtn_ = nullptr;
    QPushButton* delBtn_ = nullptr;
};

}  // namespace xsync
