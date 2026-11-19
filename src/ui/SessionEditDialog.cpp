#include "ui/SessionEditDialog.h"

#include "core/ProtocolNegotiator.h"
#include "core/SshClient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <thread>

namespace xsync {

SessionEditDialog::SessionEditDialog(const SessionProfile& initial,
                                     const QStringList& existingGroups,
                                     QWidget* parent)
    : QDialog(parent), original_(initial) {
    setWindowTitle(initial.host.isEmpty() ? QStringLiteral("Register Remote Host")
                                          : QStringLiteral("Edit Remote Host"));
    resize(520, 500);
    setupUi(existingGroups);
    populateFrom(initial);
}

void SessionEditDialog::setupUi(const QStringList& existingGroups) {
    auto* root = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);

    // Tab 1: Connection & Authentication
    auto* connTab = new QWidget(tabs);
    auto* connForm = new QFormLayout(connTab);
    connForm->setSpacing(8);

    nameEdit_ = new QLineEdit(connTab);
    nameEdit_->setPlaceholderText(QStringLiteral("e.g. GPU-Node-01"));

    groupCombo_ = new QComboBox(connTab);
    groupCombo_->setEditable(true);
    groupCombo_->addItems(existingGroups);

    hostEdit_ = new QLineEdit(connTab);
    hostEdit_->setPlaceholderText(QStringLiteral("IP or hostname (e.g. 192.168.1.10)"));

    portSpin_ = new QSpinBox(connTab);
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(22);

    auto* hostRow = new QWidget(connTab);
    auto* hostLay = new QHBoxLayout(hostRow);
    hostLay->setContentsMargins(0, 0, 0, 0);
    hostLay->addWidget(hostEdit_, 1);
    hostLay->addWidget(new QLabel(QStringLiteral("Port:"), hostRow));
    hostLay->addWidget(portSpin_);

    userEdit_ = new QLineEdit(connTab);
    userEdit_->setPlaceholderText(QStringLiteral("root / ubuntu"));

    authCombo_ = new QComboBox(connTab);
    authCombo_->addItem(QStringLiteral("Password"), static_cast<int>(AuthType::Password));
    authCombo_->addItem(QStringLiteral("Private Key File"), static_cast<int>(AuthType::PrivateKey));
    authCombo_->addItem(QStringLiteral("SSH Agent"), static_cast<int>(AuthType::SshAgent));
    connect(authCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SessionEditDialog::updateAuthVisibility);

    passwordEdit_ = new QLineEdit(connTab);
    passwordEdit_->setEchoMode(QLineEdit::Password);
    passwordEdit_->setPlaceholderText(QStringLiteral("Encrypted at rest (DPAPI / AES-GCM)"));

    auto* keyRow = new QWidget(connTab);
    auto* keyLay = new QHBoxLayout(keyRow);
    keyLay->setContentsMargins(0, 0, 0, 0);
    keyPathEdit_ = new QLineEdit(keyRow);
    keyPathEdit_->setPlaceholderText(QStringLiteral("~/.ssh/id_ed25519"));
    keyBrowseBtn_ = new QPushButton(QStringLiteral("Browse..."), keyRow);
    connect(keyBrowseBtn_, &QPushButton::clicked, this, [this]() {
        const QString p = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select SSH Private Key"),
            QDir::homePath() + QStringLiteral("/.ssh"));
        if (!p.isEmpty()) keyPathEdit_->setText(p);
    });
    keyLay->addWidget(keyPathEdit_, 1);
    keyLay->addWidget(keyBrowseBtn_);

    passphraseEdit_ = new QLineEdit(connTab);
    passphraseEdit_->setEchoMode(QLineEdit::Password);
    passphraseEdit_->setPlaceholderText(QStringLiteral("Optional key passphrase"));

    defaultRemoteEdit_ = new QLineEdit(connTab);
    defaultRemoteEdit_->setPlaceholderText(QStringLiteral("Leave empty for home dir (~)"));

    connForm->addRow(QStringLiteral("Name:"), nameEdit_);
    connForm->addRow(QStringLiteral("Group:"), groupCombo_);
    connForm->addRow(QStringLiteral("Host:"), hostRow);
    connForm->addRow(QStringLiteral("Username:"), userEdit_);
    connForm->addRow(QStringLiteral("Auth Method:"), authCombo_);
    connForm->addRow(QStringLiteral("Password:"), passwordEdit_);
    connForm->addRow(QStringLiteral("Private Key:"), keyRow);
    connForm->addRow(QStringLiteral("Key Passphrase:"), passphraseEdit_);
    connForm->addRow(QStringLiteral("Default Remote Path:"), defaultRemoteEdit_);
    tabs->addTab(connTab, QStringLiteral("Connection"));

    // Tab 2: Transfer & Protocol Policy
    auto* protoTab = new QWidget(tabs);
    auto* protoForm = new QFormLayout(protoTab);
    protoForm->setSpacing(8);

    protoCombo_ = new QComboBox(protoTab);
    protoCombo_->addItem(protocolPreferenceToString(ProtocolPreference::Auto),
                         static_cast<int>(ProtocolPreference::Auto));
    protoCombo_->addItem(protocolPreferenceToString(ProtocolPreference::Rsync),
                         static_cast<int>(ProtocolPreference::Rsync));
    protoCombo_->addItem(protocolPreferenceToString(ProtocolPreference::Sftp),
                         static_cast<int>(ProtocolPreference::Sftp));
    protoCombo_->addItem(protocolPreferenceToString(ProtocolPreference::Scp),
                         static_cast<int>(ProtocolPreference::Scp));

    r2rCombo_ = new QComboBox(protoTab);
    r2rCombo_->addItem(r2rPolicyToString(RemoteToRemotePolicy::DirectThenRelay),
                       static_cast<int>(RemoteToRemotePolicy::DirectThenRelay));
    r2rCombo_->addItem(r2rPolicyToString(RemoteToRemotePolicy::RelayOnly),
                       static_cast<int>(RemoteToRemotePolicy::RelayOnly));
    r2rCombo_->addItem(r2rPolicyToString(RemoteToRemotePolicy::DirectOnly),
                       static_cast<int>(RemoteToRemotePolicy::DirectOnly));

    forwardAgentCheck_ = new QCheckBox(
        QStringLiteral("Enable SSH Agent Forwarding (helps direct Remote ↔ Remote)"), protoTab);
    keepAliveSpin_ = new QSpinBox(protoTab);
    keepAliveSpin_->setRange(0, 300);
    keepAliveSpin_->setSuffix(QStringLiteral(" sec"));
    keepAliveSpin_->setValue(30);

    protoForm->addRow(QStringLiteral("Protocol Priority:"), protoCombo_);
    protoForm->addRow(QStringLiteral("Remote ↔ Remote:"), r2rCombo_);
    protoForm->addRow(QStringLiteral("Keepalive:"), keepAliveSpin_);
    protoForm->addRow(QString(), forwardAgentCheck_);
    tabs->addTab(protoTab, QStringLiteral("Protocols"));

    // Tab 3: JumpHost / Bastion
    auto* jumpTab = new QWidget(tabs);
    auto* jumpRoot = new QVBoxLayout(jumpTab);
    useJumpCheck_ = new QCheckBox(QStringLiteral("Connect through SSH JumpHost / Bastion"), jumpTab);
    connect(useJumpCheck_, &QCheckBox::toggled, this, &SessionEditDialog::updateJumpVisibility);
    jumpRoot->addWidget(useJumpCheck_);

    jumpContainer_ = new QWidget(jumpTab);
    auto* jumpForm = new QFormLayout(jumpContainer_);
    jumpHostEdit_ = new QLineEdit(jumpContainer_);
    jumpPortSpin_ = new QSpinBox(jumpContainer_);
    jumpPortSpin_->setRange(1, 65535);
    jumpPortSpin_->setValue(22);
    jumpUserEdit_ = new QLineEdit(jumpContainer_);
    jumpAuthCombo_ = new QComboBox(jumpContainer_);
    jumpAuthCombo_->addItem(QStringLiteral("Password"), static_cast<int>(AuthType::Password));
    jumpAuthCombo_->addItem(QStringLiteral("Private Key File"), static_cast<int>(AuthType::PrivateKey));
    jumpAuthCombo_->addItem(QStringLiteral("SSH Agent"), static_cast<int>(AuthType::SshAgent));
    jumpPasswordEdit_ = new QLineEdit(jumpContainer_);
    jumpPasswordEdit_->setEchoMode(QLineEdit::Password);
    jumpKeyEdit_ = new QLineEdit(jumpContainer_);
    jumpPassphraseEdit_ = new QLineEdit(jumpContainer_);
    jumpPassphraseEdit_->setEchoMode(QLineEdit::Password);

    jumpForm->addRow(QStringLiteral("Jump Host:"), jumpHostEdit_);
    jumpForm->addRow(QStringLiteral("Jump Port:"), jumpPortSpin_);
    jumpForm->addRow(QStringLiteral("Jump Username:"), jumpUserEdit_);
    jumpForm->addRow(QStringLiteral("Jump Auth:"), jumpAuthCombo_);
    jumpForm->addRow(QStringLiteral("Jump Password:"), jumpPasswordEdit_);
    jumpForm->addRow(QStringLiteral("Jump Key Path:"), jumpKeyEdit_);
    jumpForm->addRow(QStringLiteral("Jump Passphrase:"), jumpPassphraseEdit_);
    jumpRoot->addWidget(jumpContainer_);
    jumpRoot->addStretch(1);
    tabs->addTab(jumpTab, QStringLiteral("JumpHost"));

    root->addWidget(tabs, 1);

    // Live Test Connection row
    auto* testRow = new QHBoxLayout();
    testBtn_ = new QPushButton(QStringLiteral("Test & Probe Protocols"), this);
    connect(testBtn_, &QPushButton::clicked, this, &SessionEditDialog::testConnection);
    testResultLabel_ = new QLabel(this);
    testResultLabel_->setWordWrap(true);
    testRow->addWidget(testBtn_);
    testRow->addWidget(testResultLabel_, 1);
    root->addLayout(testRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SessionEditDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void SessionEditDialog::populateFrom(const SessionProfile& p) {
    nameEdit_->setText(p.name);
    groupCombo_->setCurrentText(p.group.isEmpty() ? QStringLiteral("Default") : p.group);
    hostEdit_->setText(p.host);
    portSpin_->setValue(p.port);
    userEdit_->setText(p.username);
    authCombo_->setCurrentIndex(authCombo_->findData(static_cast<int>(p.authType)));
    passwordEdit_->setText(p.password);
    keyPathEdit_->setText(p.privateKeyPath);
    passphraseEdit_->setText(p.passphrase);
    defaultRemoteEdit_->setText(p.defaultRemotePath);

    protoCombo_->setCurrentIndex(protoCombo_->findData(static_cast<int>(p.protocolPreference)));
    r2rCombo_->setCurrentIndex(r2rCombo_->findData(static_cast<int>(p.r2rPolicy)));
    forwardAgentCheck_->setChecked(p.forwardAgent);
    keepAliveSpin_->setValue(p.keepAliveSec);

    useJumpCheck_->setChecked(p.useJumpHost);
    jumpHostEdit_->setText(p.jumpHost);
    jumpPortSpin_->setValue(p.jumpPort);
    jumpUserEdit_->setText(p.jumpUsername);
    jumpAuthCombo_->setCurrentIndex(jumpAuthCombo_->findData(static_cast<int>(p.jumpAuthType)));
    jumpPasswordEdit_->setText(p.jumpPassword);
    jumpKeyEdit_->setText(p.jumpPrivateKeyPath);
    jumpPassphraseEdit_->setText(p.jumpPassphrase);

    updateAuthVisibility();
    updateJumpVisibility();
}

void SessionEditDialog::updateAuthVisibility() {
    const auto auth = static_cast<AuthType>(authCombo_->currentData().toInt());
    passwordEdit_->setEnabled(auth == AuthType::Password);
    keyPathEdit_->setEnabled(auth == AuthType::PrivateKey);
    keyBrowseBtn_->setEnabled(auth == AuthType::PrivateKey);
    passphraseEdit_->setEnabled(auth == AuthType::PrivateKey);
}

void SessionEditDialog::updateJumpVisibility() {
    jumpContainer_->setEnabled(useJumpCheck_->isChecked());
}

SessionProfile SessionEditDialog::profile() const {
    SessionProfile p = original_;
    if (p.id.isEmpty()) p.id = SessionProfile::createNew().id;
    p.name = nameEdit_->text().trimmed();
    p.group = groupCombo_->currentText().trimmed();
    if (p.group.isEmpty()) p.group = QStringLiteral("Default");
    p.host = hostEdit_->text().trimmed();
    p.port = static_cast<uint16_t>(portSpin_->value());
    p.username = userEdit_->text().trimmed();
    p.authType = static_cast<AuthType>(authCombo_->currentData().toInt());
    p.password = passwordEdit_->text();
    p.privateKeyPath = keyPathEdit_->text().trimmed();
    p.passphrase = passphraseEdit_->text();
    p.defaultRemotePath = defaultRemoteEdit_->text().trimmed();

    p.protocolPreference = static_cast<ProtocolPreference>(protoCombo_->currentData().toInt());
    p.r2rPolicy = static_cast<RemoteToRemotePolicy>(r2rCombo_->currentData().toInt());
    p.forwardAgent = forwardAgentCheck_->isChecked();
    p.keepAliveSec = keepAliveSpin_->value();

    p.useJumpHost = useJumpCheck_->isChecked();
    p.jumpHost = jumpHostEdit_->text().trimmed();
    p.jumpPort = static_cast<uint16_t>(jumpPortSpin_->value());
    p.jumpUsername = jumpUserEdit_->text().trimmed();
    p.jumpAuthType = static_cast<AuthType>(jumpAuthCombo_->currentData().toInt());
    p.jumpPassword = jumpPasswordEdit_->text();
    p.jumpPrivateKeyPath = jumpKeyEdit_->text().trimmed();
    p.jumpPassphrase = jumpPassphraseEdit_->text();
    return p;
}

void SessionEditDialog::testConnection() {
    SessionProfile p = profile();
    if (!p.isValid()) {
        testResultLabel_->setText(QStringLiteral("Please fill in Host and Username first."));
        return;
    }
    testBtn_->setEnabled(false);
    testResultLabel_->setStyleSheet(QStringLiteral("color: #facc15;"));
    testResultLabel_->setText(QStringLiteral("Connecting & probing capabilities..."));

    QPointer<SessionEditDialog> self(this);
    std::thread([self, p]() {
        SshClient client;
        QString err;
        bool ok = client.connectToHost(p, err, nullptr, 10000);
        EndpointCapabilities caps;
        if (ok) {
            caps = ProtocolNegotiator::probeRemote(client);
            client.disconnect();
        }
        if (!self) return;
        QMetaObject::invokeMethod(
            self,
            [self, ok, err, caps]() {
                if (!self) return;
                self->testBtn_->setEnabled(true);
                if (ok) {
                    self->testResultLabel_->setStyleSheet(QStringLiteral("color: #4ade80;"));
                    self->testResultLabel_->setText(
                        QStringLiteral("Success! OS: %1 | Protocols: %2")
                            .arg(caps.osHint.isEmpty() ? QStringLiteral("POSIX") : caps.osHint,
                                 caps.summaryBadge()));
                } else {
                    self->testResultLabel_->setStyleSheet(QStringLiteral("color: #f87171;"));
                    self->testResultLabel_->setText(err);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void SessionEditDialog::accept() {
    SessionProfile p = profile();
    if (!p.isValid()) {
        QMessageBox::warning(this, QStringLiteral("Incomplete Host"),
                             QStringLiteral("Host and Username are required."));
        return;
    }
    QDialog::accept();
}

}  // namespace xsync
