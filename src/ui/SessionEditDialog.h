#pragma once

#include "core/Types.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace xsync {

class SessionEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit SessionEditDialog(const SessionProfile& initial,
                               const QStringList& existingGroups,
                               QWidget* parent = nullptr);

    SessionProfile profile() const;

private:
    void setupUi(const QStringList& existingGroups);
    void populateFrom(const SessionProfile& p);
    void updateAuthVisibility();
    void updateJumpVisibility();
    void testConnection();
    void accept() override;

    SessionProfile original_;

    QLineEdit* nameEdit_ = nullptr;
    QComboBox* groupCombo_ = nullptr;
    QLineEdit* hostEdit_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QLineEdit* userEdit_ = nullptr;
    QComboBox* authCombo_ = nullptr;
    QLineEdit* passwordEdit_ = nullptr;
    QLineEdit* keyPathEdit_ = nullptr;
    QPushButton* keyBrowseBtn_ = nullptr;
    QLineEdit* passphraseEdit_ = nullptr;
    QLineEdit* defaultRemoteEdit_ = nullptr;
    QComboBox* protoCombo_ = nullptr;
    QComboBox* r2rCombo_ = nullptr;
    QCheckBox* forwardAgentCheck_ = nullptr;
    QSpinBox* keepAliveSpin_ = nullptr;

    // JumpHost fields
    QCheckBox* useJumpCheck_ = nullptr;
    QWidget* jumpContainer_ = nullptr;
    QLineEdit* jumpHostEdit_ = nullptr;
    QSpinBox* jumpPortSpin_ = nullptr;
    QLineEdit* jumpUserEdit_ = nullptr;
    QComboBox* jumpAuthCombo_ = nullptr;
    QLineEdit* jumpPasswordEdit_ = nullptr;
    QLineEdit* jumpKeyEdit_ = nullptr;
    QLineEdit* jumpPassphraseEdit_ = nullptr;

    QPushButton* testBtn_ = nullptr;
    QLabel* testResultLabel_ = nullptr;
};

}  // namespace xsync
