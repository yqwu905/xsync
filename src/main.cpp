#include "core/SessionStore.h"
#include "core/SshClient.h"
#include "core/TransferManager.h"
#include "core/Types.h"
#include "ui/MainWindow.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QFile>
#include <QPixmap>
#include <QSettings>
#include <QTimer>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// Built-in one-shot SSH_ASKPASS responder for rsync password authentication.
// Executes BEFORE QApplication initializes (< 1 ms, zero GUI overhead) and
// immediately unlinks the 0600 one-time token file after reading it.
int handleAskpassModeIfActive() {
    if (!qEnvironmentVariableIsSet("XSYNC_ASKPASS_MODE") ||
        qEnvironmentVariable("XSYNC_ASKPASS_MODE") != QLatin1String("1")) {
        return -1;
    }
    const QString tokenPath = qEnvironmentVariable("XSYNC_ASKPASS_TOKEN_FILE");
    if (tokenPath.isEmpty()) return 1;

    QFile f(tokenPath);
    if (!f.open(QIODevice::ReadOnly)) return 1;
    QByteArray secret = f.readAll();
    f.close();
    QFile::remove(tokenPath);

    secret.append('\n');
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
        DWORD written = 0;
        WriteFile(hOut, secret.constData(), static_cast<DWORD>(secret.size()), &written, nullptr);
    }
#else
    std::fwrite(secret.constData(), 1, static_cast<size_t>(secret.size()), stdout);
    std::fflush(stdout);
#endif
    // Zero sensitive memory before exit
    secret.fill('\0');
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Fast-path askpass mode before creating any Qt GUI objects
    if (int rc = handleAskpassModeIfActive(); rc >= 0) {
        return rc;
    }

    xsync::SshGlobalInit sshInit;

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("xsync"));
    app.setOrganizationName(QStringLiteral("xsync"));
    app.setApplicationVersion(QStringLiteral("1.0.1"));
    xsync::ThemeManager::initialize(app);

    qRegisterMetaType<xsync::PathBookmark>("xsync::PathBookmark");
    qRegisterMetaType<xsync::FileEntry>("xsync::FileEntry");
    qRegisterMetaType<std::vector<xsync::FileEntry>>("std::vector<xsync::FileEntry>");
    qRegisterMetaType<xsync::EndpointCapabilities>("xsync::EndpointCapabilities");
    qRegisterMetaType<xsync::EndpointSpec>("xsync::EndpointSpec");
    qRegisterMetaType<xsync::SessionProfile>("xsync::SessionProfile");
    qRegisterMetaType<xsync::TransferTask>("xsync::TransferTask");
    qRegisterMetaType<xsync::TransferBatch>("xsync::TransferBatch");
    qRegisterMetaType<xsync::TransferProtocol>("xsync::TransferProtocol");
    qRegisterMetaType<xsync::TransferStatus>("xsync::TransferStatus");

    // Apply saved theme (Dark by default)
    QSettings settings(QStringLiteral("xsync"), QStringLiteral("xsync"));
    const bool dark = settings.value(QStringLiteral("ui/darkTheme"), true).toBool();
    xsync::ThemeManager::applyTheme(dark);

    xsync::SessionStore sessionStore;
    sessionStore.load();

    xsync::TransferManager transferMgr;

    xsync::MainWindow::NewWindowFactory factory;
    factory = [&sessionStore, &transferMgr, &factory](const xsync::SessionProfile* initSession) {
        auto* win = new xsync::MainWindow(&sessionStore, &transferMgr, factory, initSession);
        win->show();
        return win;
    };

    xsync::MainWindow* firstWindow = factory(nullptr);

    // Optional headless screenshot helper for automated visual verification
    for (int i = 1; i + 1 < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--screenshot")) {
            const QString outPath = QString::fromLocal8Bit(argv[i + 1]);
            QTimer::singleShot(600, firstWindow, [firstWindow, outPath]() {
                firstWindow->grab().save(outPath);
                QApplication::quit();
            });
            break;
        }
    }

    const int rc = app.exec();
    transferMgr.shutdown();
    return rc;
}
