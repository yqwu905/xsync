#include "core/SessionStore.h"
#include "core/SshClient.h"
#include "core/TransferManager.h"
#include "core/Types.h"
#include "ui/BookmarkManagerDialog.h"
#include "ui/FilePane.h"
#include "ui/FileTableView.h"
#include "ui/IconCache.h"
#include "ui/MainWindow.h"
#include "ui/PaneTabWidget.h"
#include "ui/SessionEditDialog.h"
#include "ui/SessionManagerDialog.h"
#include "ui/ThemeManager.h"
#include "ui/TransferQueueWidget.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QtTest/QtTest>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace xsync;

namespace {

QString dumpDir() {
    return qEnvironmentVariable("XSYNC_DUMP_SCREENSHOTS_DIR");
}

void saveShot(const QPixmap& pix, const QString& fileName) {
    const QString dir = dumpDir();
    if (dir.isEmpty() || pix.isNull()) return;
    QDir().mkpath(dir);
    pix.save(QDir(dir).filePath(fileName));
}

QPixmap grabWidgetCompositeWithPopup(QWidget* baseWin, QWidget* popupWin) {
    QPixmap base = baseWin->grab();
    if (!popupWin || !popupWin->isVisible()) return base;

    const QPixmap pop = popupWin->grab();
    const QPoint offset = popupWin->mapToGlobal(QPoint(0, 0)) - baseWin->mapToGlobal(QPoint(0, 0));

    const int minX = std::min(0, offset.x());
    const int minY = std::min(0, offset.y());
    const int maxX = std::max(base.width(), offset.x() + pop.width());
    const int maxY = std::max(base.height(), offset.y() + pop.height());

    QPixmap canvas(maxX - minX, maxY - minY);
    canvas.fill(baseWin->palette().color(QPalette::Window));
    {
        QPainter p(&canvas);
        p.drawPixmap(-minX, -minY, base);
        p.drawPixmap(offset.x() - minX, offset.y() - minY, pop);
    }
    return canvas;
}

struct LuminanceStats {
    double medianY = 0.0;
    double minY = 255.0;
    double maxY = 0.0;
    double nearWhiteFraction = 0.0;
};

LuminanceStats analyzeInteriorLuminance(const QImage& raw, int inset = 3) {
    const QImage img = raw.convertToFormat(QImage::Format_RGBA8888);
    std::vector<double> ys;
    const int x0 = std::min(inset, img.width() / 4);
    const int y0 = std::min(inset, img.height() / 4);
    const int x1 = std::max(x0 + 1, img.width() - x0);
    const int y1 = std::max(y0 + 1, img.height() - y0);
    ys.reserve(static_cast<size_t>((x1 - x0) * (y1 - y0)));

    int nearWhite = 0;
    LuminanceStats st;
    for (int y = y0; y < y1; ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = x0; x < x1; ++x) {
            const int r = row[x * 4 + 0];
            const int g = row[x * 4 + 1];
            const int b = row[x * 4 + 2];
            const double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            ys.push_back(lum);
            if (lum < st.minY) st.minY = lum;
            if (lum > st.maxY) st.maxY = lum;
            if (r > 235 && g > 235 && b > 235) ++nearWhite;
        }
    }
    if (!ys.empty()) {
        std::nth_element(ys.begin(), ys.begin() + static_cast<ptrdiff_t>(ys.size() / 2), ys.end());
        st.medianY = ys[ys.size() / 2];
        st.nearWhiteFraction = static_cast<double>(nearWhite) / static_cast<double>(ys.size());
    }
    return st;
}

void verifySpinBoxFitsMax(QSpinBox* sp, int maxVal, const char* name) {
    QVERIFY2(sp != nullptr, name);
    const int orig = sp->value();
    sp->setValue(maxVal);
    QCoreApplication::processEvents();
    QLineEdit* le = sp->findChild<QLineEdit*>();
    QVERIFY2(le != nullptr, name);
    const int innerW = le->contentsRect().width();
    const int textAdv = le->fontMetrics().horizontalAdvance(le->text());
    QVERIFY2(innerW >= textAdv + 4,
             qPrintable(QStringLiteral("SpinBox '%1' text '%2' clipped: innerW=%3 < adv(%4)+4")
                            .arg(QString::fromLatin1(name), le->text()).arg(innerW).arg(textAdv)));
    sp->setValue(orig);
    QCoreApplication::processEvents();
}

void verifyComboGeometry(QComboBox* combo, bool checkEditFitsLongest) {
    QVERIFY(combo != nullptr);
    QStyleOptionComboBox opt;
    opt.initFrom(combo);
    opt.editable = combo->isEditable();
    opt.frame = combo->hasFrame();
    opt.subControls = QStyle::SC_All;
    const QRect editSC = combo->style()->subControlRect(
        QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, combo);
    const QRect arrowSC = combo->style()->subControlRect(
        QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxArrow, combo);
    const int deadGap = arrowSC.left() - editSC.right();
    QVERIFY2(deadGap >= 0 && deadGap <= 10,
             qPrintable(QStringLiteral("Combo '%1' deadGap=%2 (expected 0..10; editRight=%3 arrowLeft=%4)")
                            .arg(combo->objectName()).arg(deadGap).arg(editSC.right()).arg(arrowSC.left())));
    if (combo->isEditable() && combo->lineEdit()) {
        const QRect leGeo = combo->lineEdit()->geometry();
        QVERIFY2(leGeo.right() < arrowSC.left(),
                 qPrintable(QStringLiteral("Editable combo '%1' inner QLineEdit right=%2 overlaps arrowLeft=%3")
                                .arg(combo->objectName()).arg(leGeo.right()).arg(arrowSC.left())));
    }
    if (checkEditFitsLongest) {
        int maxAdv = 0;
        for (int i = 0; i < combo->count(); ++i) {
            maxAdv = std::max(maxAdv, combo->fontMetrics().horizontalAdvance(combo->itemText(i)));
        }
        QVERIFY2(editSC.width() >= maxAdv + 8,
                 qPrintable(QStringLiteral("Combo '%1' editSC.width()=%2 < maxAdv(%3)+8")
                                .arg(combo->objectName()).arg(editSC.width()).arg(maxAdv)));
    }
}

void verifyComboPopup(QComboBox* combo,
                      QWidget* hostWindow,
                      bool dark,
                      const QString& shotName = {},
                      const QString& popupShotName = {}) {
    QVERIFY2(combo != nullptr, "Null QComboBox");
    QVERIFY2(qobject_cast<QListView*>(combo->view()) != nullptr,
             qPrintable(QStringLiteral("Combo '%1' does not use QListView").arg(combo->objectName())));
    QVERIFY2(qobject_cast<QStyledItemDelegate*>(combo->view()->itemDelegate()) != nullptr,
             qPrintable(QStringLiteral("Combo '%1' does not use QStyledItemDelegate").arg(combo->objectName())));

    combo->showPopup();
    QWidget* popupWin = combo->view()->window();
    const bool visImmediate = popupWin && popupWin->isVisible();
    QCoreApplication::processEvents();
    const bool visAfterEvents = popupWin && popupWin->isVisible();
    QTest::qWait(50);

    QVERIFY2(popupWin != nullptr && popupWin != combo->window(), "Combo popup window missing");
    QVERIFY2(popupWin->isVisible(),
             qPrintable(QStringLiteral("Combo '%1' popup not visible (imm=%2 afterEv=%3 activeWin=%4 activePop=%5)")
                            .arg(combo->objectName())
                            .arg(visImmediate).arg(visAfterEvents)
                            .arg(qApp->activeWindow() ? qApp->activeWindow()->metaObject()->className() : "null")
                            .arg(qApp->activePopupWidget() ? qApp->activePopupWidget()->objectName() : "null")));
    QVERIFY2(!popupWin->testAttribute(Qt::WA_TranslucentBackground),
             "Popup container must NOT set WA_TranslucentBackground");
    QVERIFY2(popupWin->width() >= combo->width() - 2,
             qPrintable(QStringLiteral("Combo '%1' popup width %2 < combo width %3")
                            .arg(combo->objectName())
                            .arg(popupWin->width())
                            .arg(combo->width())));
    QVERIFY2(popupWin->height() >= 24,
             qPrintable(QStringLiteral("Combo '%1' popup height %2 < 24")
                            .arg(combo->objectName())
                            .arg(popupWin->height())));

    for (int i = 0; i < combo->count(); ++i) {
        const QRect r = combo->view()->visualRect(combo->model()->index(i, 0));
        QVERIFY2(r.height() >= 22,
                 qPrintable(QStringLiteral("Combo '%1' row %2 height %3 < 22")
                                .arg(combo->objectName())
                                .arg(i)
                                .arg(r.height())));
    }

    const QPixmap popPix = popupWin->grab();
    const QImage popImg = popPix.toImage().convertToFormat(QImage::Format_RGBA8888);
    const LuminanceStats st = analyzeInteriorLuminance(popImg, 3);

    if (dark) {
        QVERIFY2(st.medianY < 70.0,
                 qPrintable(QStringLiteral("Dark combo '%1' popup leaked light palette (medianY=%2)")
                                .arg(combo->objectName())
                                .arg(st.medianY)));
        QVERIFY2(st.nearWhiteFraction < 0.05,
                 qPrintable(QStringLiteral("Dark combo '%1' popup has %2% near-white pixels")
                                .arg(combo->objectName())
                                .arg(st.nearWhiteFraction * 100.0)));
    } else {
        QVERIFY2(st.medianY > 210.0,
                 qPrintable(QStringLiteral("Light combo '%1' popup too dark (medianY=%2)")
                                .arg(combo->objectName())
                                .arg(st.medianY)));
        QVERIFY2(st.minY < 160.0,
                 qPrintable(QStringLiteral("Light combo '%1' popup missing dark text/highlight contrast (minY=%2)")
                                .arg(combo->objectName())
                                .arg(st.minY)));
    }

    // Verify no pure-black (0,0,0) uncomposited corner artifacts
    if (popImg.width() >= 4 && popImg.height() >= 4) {
        const QColor cTL = popImg.pixelColor(0, 0);
        const QColor cTR = popImg.pixelColor(popImg.width() - 1, 0);
        const QColor cBL = popImg.pixelColor(0, popImg.height() - 1);
        const QColor cBR = popImg.pixelColor(popImg.width() - 1, popImg.height() - 1);
        auto isPureBlack = [](const QColor& c) { return c.red() == 0 && c.green() == 0 && c.blue() == 0; };
        QVERIFY2(!(isPureBlack(cTL) && isPureBlack(cTR) && isPureBlack(cBL) && isPureBlack(cBR)),
                 qPrintable(QStringLiteral("Combo '%1' popup has pure-black corner artifacts").arg(combo->objectName())));
    }

    if (!shotName.isEmpty() && hostWindow) {
        saveShot(grabWidgetCompositeWithPopup(hostWindow, popupWin), shotName);
    }
    if (!popupShotName.isEmpty()) {
        saveShot(popPix, popupShotName);
    }

    combo->hidePopup();
    QCoreApplication::processEvents();
}

void createMockFile(const QString& path, const QByteArray& content) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(content);
    }
}

void seedSessionStore(SessionStore& store, const QString& localDir) {
    SessionProfile s1 = SessionProfile::createNew();
    s1.name = QStringLiteral("GPU-Node-01");
    s1.group = QStringLiteral("GPU Cluster");
    s1.host = QStringLiteral("10.0.10.21");
    s1.port = 22;
    s1.username = QStringLiteral("mlops");
    s1.authType = AuthType::PrivateKey;
    s1.privateKeyPath = QStringLiteral("~/.ssh/id_ed25519");
    s1.protocolPreference = ProtocolPreference::Auto;
    s1.r2rPolicy = RemoteToRemotePolicy::DirectThenRelay;

    SessionProfile s2 = SessionProfile::createNew();
    s2.name = QStringLiteral("Prod-Web-01");
    s2.group = QStringLiteral("Production");
    s2.host = QStringLiteral("192.168.50.12");
    s2.port = 2222;
    s2.username = QStringLiteral("deploy");
    s2.authType = AuthType::Password;
    s2.protocolPreference = ProtocolPreference::Rsync;
    s2.r2rPolicy = RemoteToRemotePolicy::RelayOnly;
    s2.useJumpHost = true;
    s2.jumpHost = QStringLiteral("bastion.corp.local");
    s2.jumpPort = 2200;
    s2.jumpUsername = QStringLiteral("jump");

    SessionProfile s3 = SessionProfile::createNew();
    s3.name = QStringLiteral("Storage-NAS");
    s3.group = QStringLiteral("Production");
    s3.host = QStringLiteral("192.168.50.99");
    s3.port = 22;
    s3.username = QStringLiteral("backup");
    s3.authType = AuthType::SshAgent;
    s3.protocolPreference = ProtocolPreference::Sftp;
    s3.r2rPolicy = RemoteToRemotePolicy::DirectOnly;

    store.upsertSession(s1);
    store.upsertSession(s2);
    store.upsertSession(s3);

    const EndpointSpec localEp = EndpointSpec::local();
    store.addBookmark(localEp, {QStringLiteral("Workspace"), localDir});
    store.addBookmark(localEp, {QStringLiteral("Checkpoints"), localDir + QStringLiteral("/checkpoints")});
    store.addBookmark(localEp, {QStringLiteral("Training Logs"), localDir + QStringLiteral("/logs")});
}

void seedTransferQueue(TransferManager& mgr, const QString& tmpRoot) {
    const QString srcBatch = tmpRoot + QStringLiteral("/dataset_batch");
    const QString dstDir = tmpRoot + QStringLiteral("/dst_out");
    QDir().mkpath(srcBatch);
    QDir().mkpath(dstDir);

    createMockFile(srcBatch + QStringLiteral("/weights_v1.bin"), QByteArray(128 * 1024, 'A'));
    createMockFile(srcBatch + QStringLiteral("/metrics.json"), QByteArray("{\"loss\":0.042}\n"));
    createMockFile(srcBatch + QStringLiteral("/tokenizer.model"), QByteArray(32 * 1024, 'T'));

    mgr.setPaused(true);

    // 1. Directory batch (expanded into 3 children: 1 completed, 1 paused, 1 queued)
    TransferTask dirTask;
    dirTask.sourceEndpoint = EndpointSpec::local();
    dirTask.destEndpoint = EndpointSpec::local();
    dirTask.sourcePath = srcBatch;
    dirTask.destDir = dstDir;
    dirTask.itemName = QStringLiteral("dataset_batch");
    dirTask.isDir = true;
    const uint64_t batchId = mgr.enqueueTask(dirTask);

    QTRY_VERIFY_WITH_TIMEOUT(mgr.tasksInBatch(batchId).size() == 3, 5000);
    const auto kids = mgr.tasksInBatch(batchId);
    mgr.pauseTask(kids[0].id);

    // 2. Standalone failed task for error contrast verification in Dark & Light modes
    TransferTask badTask;
    badTask.sourceEndpoint = EndpointSpec::local();
    badTask.destEndpoint = EndpointSpec::local();
    badTask.sourcePath = tmpRoot + QStringLiteral("/missing_checkpoint.pt");
    badTask.destDir = dstDir;
    badTask.itemName = QStringLiteral("missing_checkpoint.pt");
    const uint64_t badId = mgr.enqueueTask(badTask);

    // Unpause briefly so non-paused files complete and badTask fails cleanly
    mgr.setPaused(false);
    QTRY_VERIFY_WITH_TIMEOUT(
        mgr.findTask(badId) && mgr.findTask(badId)->status == TransferStatus::Failed &&
            mgr.findBatch(batchId) && mgr.findBatch(batchId)->completedFiles >= 2,
        5000);
}

}  // namespace

class TestUi : public QObject {
    Q_OBJECT

private:
    void runFullSuiteForTheme(bool dark) {
        const QString tag = dark ? QStringLiteral("dark") : QStringLiteral("light");
        ThemeManager::applyTheme(dark);
        QCOMPARE(ThemeManager::isDark(), dark);
        QCOMPARE(IconCache::isDarkTheme(), dark);
        QCoreApplication::processEvents();

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString workDir = tmp.filePath(QStringLiteral("workspace"));
        QDir().mkpath(workDir + QStringLiteral("/logs"));
        QDir().mkpath(workDir + QStringLiteral("/checkpoints"));
        createMockFile(workDir + QStringLiteral("/README.md"), QByteArray("# xsync UI test\n"));
        createMockFile(workDir + QStringLiteral("/模型权重.pt"), QByteArray(8192, '\x5a'));
        createMockFile(workDir + QStringLiteral("/train_config.yaml"), QByteArray("epochs: 100\n"));
        createMockFile(workDir + QStringLiteral("/archive_2026.tar.gz"), QByteArray(2048, '\x1f'));
        createMockFile(workDir + QStringLiteral("/preview.png"), QByteArray(1024, '\x89'));

        SessionStore store(tmp.filePath(QStringLiteral("sessions.json")));
        seedSessionStore(store, workDir);

        TransferManager transferMgr;
        seedTransferQueue(transferMgr, tmp.path());

        // -------------------------------------------------------------------
        // 1. MainWindow, Toolbars, Dual-Pane Alignment, FilePane & TransferQueue
        // -------------------------------------------------------------------
        {
            std::unique_ptr<MainWindow> win(new MainWindow(&store, &transferMgr, {}));
            win->setAttribute(Qt::WA_DeleteOnClose, false);
            win->resize(1340, 840);
            win->show();
            QCoreApplication::processEvents();
            QTest::qWait(40);

            // Navigate left pane into our mock workspace (which is bookmarked!)
            FilePane* leftPane = win->leftTabs()->currentFilePane();
            QVERIFY(leftPane != nullptr);
            leftPane->navigateTo(workDir, true);
            FilePane* rightLocalPane = win->rightTabs()->currentFilePane();
            QVERIFY(rightLocalPane != nullptr);
            rightLocalPane->navigateTo(workDir, false);
            QCoreApplication::processEvents();
            QTest::qWait(30);

            // Expand TransferQueue folder batch so completed/paused/failed rows render clearly
            auto* queueWidget = win->findChild<TransferQueueWidget*>();
            QVERIFY(queueWidget != nullptr);
            auto* queueTree = queueWidget->findChild<QTreeView*>();
            QVERIFY(queueTree != nullptr);
            queueTree->expandAll();
            QCoreApplication::processEvents();
            QTest::qWait(30);

            // Shot 01: MainWindow full view
            saveShot(win->grab(), QStringLiteral("01_main_window_%1.png").arg(tag));

            // Main Toolbar height & alignment checks
            QToolBar* mainTb = nullptr;
            QToolBar* qcTb = nullptr;
            for (QToolBar* tb : win->findChildren<QToolBar*>()) {
                if (tb->windowTitle() == QLatin1String("Main")) mainTb = tb;
                else if (tb->windowTitle() == QLatin1String("Quick Connect")) qcTb = tb;
            }
            QVERIFY(mainTb != nullptr);
            QVERIFY(qcTb != nullptr);

            const auto tbButtons = mainTb->findChildren<QToolButton*>();
            int visibleTbButtons = 0;
            int refTbH = -1;
            for (QToolButton* btn : tbButtons) {
                if (!btn->isVisible() || btn->objectName() == QLatin1String("qt_toolbar_ext_button")) continue;
                ++visibleTbButtons;
                if (refTbH < 0) refTbH = btn->height();
                QVERIFY2(std::abs(btn->height() - refTbH) <= 2,
                         qPrintable(QStringLiteral("Main toolbar button '%1' height %2 != %3")
                                        .arg(btn->text()).arg(btn->height()).arg(refTbH)));
            }
            QCOMPARE(visibleTbButtons, 4);

            const auto tbCombos = mainTb->findChildren<QComboBox*>();
            QCOMPARE(tbCombos.size(), 2);
            QCOMPARE(tbCombos[0]->objectName(), QStringLiteral("protoPrefCombo"));
            QCOMPARE(tbCombos[1]->objectName(), QStringLiteral("r2rPolicyCombo"));
            QVERIFY2(std::abs(tbCombos[0]->height() - tbCombos[1]->height()) <= 1,
                     "Toolbar combobox heights mismatch");
            QVERIFY2(std::abs(tbCombos[0]->height() - refTbH) <= 2,
                     qPrintable(QStringLiteral("Toolbar combo height %1 vs button height %2")
                                    .arg(tbCombos[0]->height()).arg(refTbH)));
            verifyComboGeometry(tbCombos[0], true);
            verifyComboGeometry(tbCombos[1], true);

            // Verify Main Toolbar combobox popups (#1 & #2) + Shot 02
            verifyComboPopup(tbCombos[0], win.get(), dark,
                             QStringLiteral("02_main_toolbar_combo_open_%1.png").arg(tag),
                             QStringLiteral("02_main_toolbar_combo_popup_only_%1.png").arg(tag));
            verifyComboPopup(tbCombos[1], win.get(), dark);

            // Quick Connect bar height, 65535 spinbox inner-width fit, & button non-truncation
            auto* qcSpin = qcTb->findChild<QSpinBox*>();
            QVERIFY(qcSpin != nullptr);
            verifySpinBoxFitsMax(qcSpin, 65535, "qcPortSpin");
            const auto qcEdits = qcTb->findChildren<QLineEdit*>(QString(), Qt::FindDirectChildrenOnly);
            const auto qcBtns = qcTb->findChildren<QPushButton*>();
            QCOMPARE(qcEdits.size(), 3);
            QCOMPARE(qcBtns.size(), 3);
            for (QLineEdit* le : qcEdits) {
                QVERIFY2(std::abs(le->height() - qcSpin->height()) <= 2,
                         qPrintable(QStringLiteral("QuickConnect edit height %1 vs spin %2")
                                        .arg(le->height()).arg(qcSpin->height())));
            }
            for (QPushButton* btn : qcBtns) {
                QVERIFY2(std::abs(btn->height() - qcSpin->height()) <= 2,
                         qPrintable(QStringLiteral("QuickConnect button '%1' height %2 vs spin %3")
                                        .arg(btn->text()).arg(btn->height()).arg(qcSpin->height())));
                const int adv = btn->fontMetrics().horizontalAdvance(btn->text());
                QVERIFY2(btn->width() >= adv + 22,
                         qPrintable(QStringLiteral("QuickConnect button '%1' width %2 too narrow for text %3")
                                        .arg(btn->text()).arg(btn->width()).arg(adv)));
            }

            // 1024x768 MainWindow toolbar & Quick Connect fit regression (zero overflow / clipping)
            {
                win->resize(1024, 768);
                qcSpin->setValue(65535);
                QCoreApplication::processEvents();
                QTest::qWait(40);

                auto verifyToolbarFits1024 = [](QToolBar* tb, const char* name) {
                    auto dumpLayout = [tb]() {
                        QStringList parts;
                        parts << QStringLiteral("tbW=%1").arg(tb->width());
                        for (QWidget* w : tb->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
                            parts << QStringLiteral("%1(%2,%3:%4x%5,vis=%6)")
                                         .arg(w->objectName().isEmpty() ? QString::fromLatin1(w->metaObject()->className())
                                                                        : w->objectName())
                                         .arg(w->x()).arg(w->geometry().right())
                                         .arg(w->width()).arg(w->height())
                                         .arg(w->isVisible() ? 1 : 0);
                        }
                        return parts.join(QStringLiteral(" "));
                    };
                    if (auto* ext = tb->findChild<QAbstractButton*>(QStringLiteral("qt_toolbar_ext_button"))) {
                        QVERIFY2(!ext->isVisible(),
                                 qPrintable(QStringLiteral("Toolbar '%1' overflowed at 1024px: %2")
                                                .arg(QString::fromLatin1(name), dumpLayout())));
                    }
                    const auto directChildren = tb->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
                    for (QWidget* w : directChildren) {
                        if (w->objectName() == QLatin1String("qt_toolbar_ext_button")) continue;
                        if (qobject_cast<QToolButton*>(w) || qobject_cast<QPushButton*>(w) ||
                            qobject_cast<QComboBox*>(w) || qobject_cast<QLineEdit*>(w) ||
                            qobject_cast<QSpinBox*>(w) || qobject_cast<QLabel*>(w)) {
                            QVERIFY2(w->isVisible(),
                                     qPrintable(QStringLiteral("Toolbar '%1' child '%2' hidden at 1024px: %3")
                                                    .arg(QString::fromLatin1(name), w->objectName(), dumpLayout())));
                            QVERIFY2(w->geometry().right() < tb->width(),
                                     qPrintable(QStringLiteral("Toolbar '%1' child '%2' right=%3 >= width=%4: %5")
                                                    .arg(QString::fromLatin1(name), w->objectName())
                                                    .arg(w->geometry().right()).arg(tb->width()).arg(dumpLayout())));
                        }
                    }
                };
                verifyToolbarFits1024(mainTb, "Main");
                verifyToolbarFits1024(qcTb, "Quick Connect");
                QVERIFY(tbCombos[1]->isVisible());

                auto* openRightBtn = qcTb->findChild<QPushButton*>(QStringLiteral("primaryBtn"));
                QVERIFY(openRightBtn != nullptr && openRightBtn->isVisible());
                const int openRightAdv = openRightBtn->fontMetrics().horizontalAdvance(openRightBtn->text());
                QVERIFY2(openRightBtn->width() >= openRightAdv + 22,
                         qPrintable(QStringLiteral("1024px Open Right width %1 < adv(%2)+22")
                                        .arg(openRightBtn->width()).arg(openRightAdv)));
                verifySpinBoxFitsMax(qcSpin, 65535, "qcPortSpin@1024");
                qcSpin->setValue(65535);
                QCoreApplication::processEvents();
                saveShot(win->grab(), QStringLiteral("13_main_window_1024_65535_%1.png").arg(tag));

                qcSpin->setValue(22);
                win->resize(1340, 840);
                QCoreApplication::processEvents();
                QTest::qWait(30);
            }

            // PaneTabWidget corner buttons height parity
            auto* leftAddLocal = win->leftTabs()->findChild<QToolButton*>(QStringLiteral("addLocalBtn"));
            auto* leftAddRemote = win->leftTabs()->findChild<QToolButton*>(QStringLiteral("addRemoteBtn"));
            auto* rightAddLocal = win->rightTabs()->findChild<QToolButton*>(QStringLiteral("addLocalBtn"));
            auto* rightAddRemote = win->rightTabs()->findChild<QToolButton*>(QStringLiteral("addRemoteBtn"));
            QVERIFY(leftAddLocal && leftAddRemote && rightAddLocal && rightAddRemote);
            QVERIFY(std::abs(leftAddLocal->height() - leftAddRemote->height()) <= 1);
            QVERIFY(std::abs(leftAddLocal->height() - rightAddLocal->height()) <= 1);
            QVERIFY(std::abs(rightAddLocal->height() - rightAddRemote->height()) <= 1);
            QVERIFY2(leftAddLocal->height() <= 26,
                     qPrintable(QStringLiteral("Corner button height %1 too tall").arg(leftAddLocal->height())));

            // PaneTabWidget + Remote menu submenu memory leak regression (10 opens with 3 groups -> <= 3)
            {
                SessionStore leakStore(tmp.filePath(QStringLiteral("leak_sessions.json")));
                for (const QString& grp : {QStringLiteral("GPU Cluster"),
                                           QStringLiteral("Production"),
                                           QStringLiteral("Staging")}) {
                    SessionProfile p = SessionProfile::createNew();
                    p.name = grp + QStringLiteral("-Node");
                    p.group = grp;
                    p.host = QStringLiteral("10.0.0.1");
                    p.username = QStringLiteral("ops");
                    leakStore.upsertSession(p);
                }
                PaneTabWidget leakTabs(QStringLiteral("LeakCheck"), &leakStore, &transferMgr);
                auto* addRem = leakTabs.findChild<QToolButton*>(QStringLiteral("addRemoteBtn"));
                QVERIFY(addRem && addRem->menu());
                QMenu* rmMenu = addRem->menu();
                for (int i = 0; i < 10; ++i) {
                    rmMenu->popup(QPoint(100, 100));
                    QCoreApplication::processEvents();
                    rmMenu->hide();
                    QCoreApplication::processEvents();
                }
                const int subCount = rmMenu->findChildren<QMenu*>().size();
                QVERIFY2(subCount == 3,
                         qPrintable(QStringLiteral("PaneTabWidget +Remote leaked child QMenus: expected 3, got %1")
                                        .arg(subCount)));
            }

            // FilePane Nav Bar uniformity + 0px starBtn toggle jitter
            auto* backBtn = leftPane->findChild<QToolButton*>(QStringLiteral("backBtn"));
            auto* upBtn = leftPane->findChild<QToolButton*>(QStringLiteral("upBtn"));
            auto* homeBtn = leftPane->findChild<QToolButton*>(QStringLiteral("homeBtn"));
            auto* starBtn = leftPane->findChild<QToolButton*>(QStringLiteral("starBtnActive"));
            if (!starBtn) starBtn = leftPane->findChild<QToolButton*>(QStringLiteral("starBtn"));
            auto* bookmarksBtn = leftPane->findChild<QToolButton*>(QStringLiteral("bookmarksBtn"));
            auto* refreshBtn = leftPane->findChild<QToolButton*>(QStringLiteral("refreshBtn"));
            QVERIFY(backBtn && upBtn && homeBtn && starBtn && bookmarksBtn && refreshBtn);

            const std::vector<QToolButton*> squareBtns = {backBtn, upBtn, homeBtn, starBtn, refreshBtn};
            for (QToolButton* b : squareBtns) {
                QVERIFY2(std::abs(b->width() - b->height()) <= 1,
                         qPrintable(QStringLiteral("Nav button '%1' not square: %2x%3")
                                        .arg(b->objectName()).arg(b->width()).arg(b->height())));
                QVERIFY2(std::abs(b->height() - backBtn->height()) <= 1,
                         qPrintable(QStringLiteral("Nav button '%1' height %2 != %3")
                                        .arg(b->objectName()).arg(b->height()).arg(backBtn->height())));
            }
            QVERIFY2(std::abs(bookmarksBtn->height() - backBtn->height()) <= 1,
                     qPrintable(QStringLiteral("bookmarksBtn height %1 != %2")
                                    .arg(bookmarksBtn->height()).arg(backBtn->height())));
            const auto navEdits = leftPane->findChildren<QLineEdit*>();
            QCOMPARE(navEdits.size(), 2);
            for (QLineEdit* le : navEdits) {
                QVERIFY2(std::abs(le->height() - backBtn->height()) <= 1,
                         qPrintable(QStringLiteral("FilePane nav QLineEdit height %1 != %2")
                                        .arg(le->height()).arg(backBtn->height())));
            }

            // Star toggle 0px jitter check
            const QSize starSizeBookmarked = starBtn->size();
            QCOMPARE(starBtn->objectName(), QStringLiteral("starBtnActive"));
            store.removeBookmark(EndpointSpec::local(), workDir);
            QCoreApplication::processEvents();
            QCOMPARE(starBtn->objectName(), QStringLiteral("starBtn"));
            QCOMPARE(starBtn->size(), starSizeBookmarked);
            store.addBookmark(EndpointSpec::local(), {QStringLiteral("Workspace"), workDir});
            QCoreApplication::processEvents();
            QCOMPARE(starBtn->objectName(), QStringLiteral("starBtnActive"));
            QCOMPARE(starBtn->size(), starSizeBookmarked);

            // TransferQueueWidget control bar uniformity + 0px pauseAllBtn jitter + filterCombo popup (#3)
            const auto queueBtns = queueWidget->findChildren<QPushButton*>();
            auto* queueCombo = queueWidget->findChild<QComboBox*>(QStringLiteral("queueFilterCombo"));
            auto* queueSpin = queueWidget->findChild<QSpinBox*>(QStringLiteral("queueConcurSpin"));
            auto* pauseAllBtn = queueWidget->findChild<QPushButton*>(QStringLiteral("pauseAllBtn"));
            QCOMPARE(queueBtns.size(), 6);
            QVERIFY(queueCombo && queueSpin && pauseAllBtn);
            verifyComboGeometry(queueCombo, true);
            verifySpinBoxFitsMax(queueSpin, 8, "concurSpin");

            for (QPushButton* b : queueBtns) {
                QVERIFY2(std::abs(b->height() - queueCombo->height()) <= 2,
                         qPrintable(QStringLiteral("Queue button '%1' height %2 != combo %3")
                                        .arg(b->text()).arg(b->height()).arg(queueCombo->height())));
            }
            QVERIFY2(std::abs(queueSpin->height() - queueCombo->height()) <= 2,
                     qPrintable(QStringLiteral("Queue spin height %1 != combo %2")
                                    .arg(queueSpin->height()).arg(queueCombo->height())));

            // Zero jitter on Pause All <-> Resume All
            const int pauseW0 = pauseAllBtn->width();
            pauseAllBtn->click();
            QCoreApplication::processEvents();
            QCOMPARE(pauseAllBtn->text(), QStringLiteral("Resume All"));
            QCOMPARE(pauseAllBtn->width(), pauseW0);
            pauseAllBtn->click();
            QCoreApplication::processEvents();
            QCOMPARE(pauseAllBtn->text(), QStringLiteral("Pause All"));
            QCOMPARE(pauseAllBtn->width(), pauseW0);

            // Verify TransferQueue row height >= 26px (28px target)
            if (queueTree->model()->rowCount() > 0) {
                const QRect rowRect = queueTree->visualRect(queueTree->model()->index(0, 0));
                QVERIFY2(rowRect.height() >= 26,
                         qPrintable(QStringLiteral("TransferQueue row height %1 < 26").arg(rowRect.height())));
            }

            // Shot 03: TransferQueue filterCombo open
            verifyComboPopup(queueCombo, win.get(), dark,
                             QStringLiteral("03_transfer_queue_combo_open_%1.png").arg(tag),
                             QStringLiteral("03_transfer_queue_combo_popup_only_%1.png").arg(tag));

            // Dual-Pane Header Alignment: Left Local vs Right Remote (with visible Reconnect button)
            SessionProfile mockRemote = SessionProfile::createNew();
            mockRemote.name = QStringLiteral("GPU-Node-01");
            mockRemote.host = QStringLiteral("127.0.0.1");
            mockRemote.port = 1;  // Immediate loopback refusal; header & Reconnect button visible immediately
            mockRemote.username = QStringLiteral("mlops");
            FilePane* rightRemotePane = win->rightTabs()->openRemoteTab(mockRemote);
            QVERIFY(rightRemotePane != nullptr);
            QCoreApplication::processEvents();
            QTest::qWait(40);

            auto* leftHeader = leftPane->findChild<QFrame*>(QStringLiteral("paneHeader"));
            auto* rightHeader = rightRemotePane->findChild<QFrame*>(QStringLiteral("paneHeader"));
            auto* leftTable = leftPane->findChild<FileTableView*>();
            auto* rightTable = rightRemotePane->findChild<FileTableView*>();
            QVERIFY(leftHeader && rightHeader && leftTable && rightTable);
            QVERIFY2(std::abs(leftHeader->height() - rightHeader->height()) <= 1,
                     qPrintable(QStringLiteral("Dual-pane header skew: Left=%1 vs Right=%2")
                                    .arg(leftHeader->height()).arg(rightHeader->height())));
            const int leftTableY = leftTable->mapTo(win.get(), QPoint(0, 0)).y();
            const int rightTableY = rightTable->mapTo(win.get(), QPoint(0, 0)).y();
            QVERIFY2(std::abs(leftTableY - rightTableY) <= 1,
                     qPrintable(QStringLiteral("Dual-pane table Y skew: Left=%1 vs Right=%2")
                                    .arg(leftTableY).arg(rightTableY)));

            // Verify long remote hostname elides cleanly with tooltip while preserving 36px header height
            {
                SessionProfile longRemote = mockRemote;
                longRemote.name = QStringLiteral("gpu-cluster-prod-very-long-hostname-01.internal.example.org");
                longRemote.host = QStringLiteral("gpu-cluster-prod-very-long-hostname-01.internal.example.org");
                FilePane* longPane = win->rightTabs()->openRemoteTab(longRemote);
                QCoreApplication::processEvents();
                QTest::qWait(30);
                auto* longHdr = longPane->findChild<QFrame*>(QStringLiteral("paneHeader"));
                auto* titleLbl = longPane->findChild<QLabel*>(QStringLiteral("hostTitleLabel"));
                QVERIFY(longHdr && titleLbl);
                QCOMPARE(longHdr->height(), 36);
                QVERIFY(titleLbl->toolTip().contains(longRemote.name));
                QVERIFY2(titleLbl->text().endsWith(QChar(0x2026)) || titleLbl->text().endsWith(QStringLiteral("...")),
                         qPrintable(QStringLiteral("Long hostname did not elide cleanly: '%1'").arg(titleLbl->text())));
                win->rightTabs()->setCurrentWidget(rightRemotePane);
                QCoreApplication::processEvents();
            }

            // Shot 12: Dual-pane Local (bookmarked star active) vs Remote header alignment + open Bookmarks menu
            if (!dumpDir().isEmpty()) {
                QMenu* bmMenu = bookmarksBtn->menu();
                QVERIFY(bmMenu != nullptr);
                bmMenu->popup(bookmarksBtn->mapToGlobal(QPoint(0, bookmarksBtn->height())));
                QCoreApplication::processEvents();
                QTest::qWait(50);
                saveShot(grabWidgetCompositeWithPopup(win.get(), bmMenu),
                         QStringLiteral("12_dual_pane_header_and_star_active_%1.png").arg(tag));
                bmMenu->hide();
                QCoreApplication::processEvents();
            }

            win->close();
            QCoreApplication::processEvents();
        }

        // -------------------------------------------------------------------
        // 2. SessionManagerDialog (Shot 04 + disabled #primaryBtn + clearSelection + Light unfocused chevron)
        // -------------------------------------------------------------------
        {
            SessionManagerDialog dlg(&store);
            dlg.show();
            QCoreApplication::processEvents();
            QTest::qWait(40);

            auto* searchEdit = dlg.findChild<QLineEdit*>();
            auto* primaryBtn = dlg.findChild<QPushButton*>(QStringLiteral("primaryBtn"));
            QVERIFY(searchEdit && primaryBtn);

            // Initially no host is selected -> openRightBtn_ (#primaryBtn) MUST be disabled
            QVERIFY(!primaryBtn->isEnabled());

            // Verify disabled #primaryBtn does NOT render solid blue (#2563eb)
            const QImage disImg = primaryBtn->grab().toImage().convertToFormat(QImage::Format_RGBA8888);
            const QColor midColor = disImg.pixelColor(disImg.width() / 2, 3);
            QVERIFY2(!(midColor.blue() > 200 && midColor.red() < 60),
                     qPrintable(QStringLiteral("Disabled #primaryBtn leaked active blue RGB(%1,%2,%3)")
                                    .arg(midColor.red()).arg(midColor.green()).arg(midColor.blue())));

            // Select a host item and verify all buttons fit within dialog width & share heights
            auto* tree = dlg.findChild<QTreeWidget*>();
            QVERIFY(tree && tree->topLevelItemCount() > 0 && tree->topLevelItem(0)->childCount() > 0);
            QVERIFY2(tree->columnWidth(0) >= 120,
                     qPrintable(QStringLiteral("SessionManager Name column collapsed to %1px")
                                    .arg(tree->columnWidth(0))));
            QVERIFY2(tree->columnWidth(3) >= 196,
                     qPrintable(QStringLiteral("SessionManager Protocol Policy col %1 < 196")
                                    .arg(tree->columnWidth(3))));
            tree->setCurrentItem(tree->topLevelItem(0)->child(0));
            QCoreApplication::processEvents();
            QVERIFY(primaryBtn->isEnabled());

            // clearSelection() regression: all 6 selection-dependent footer buttons MUST disable
            tree->clearSelection();
            QCoreApplication::processEvents();
            for (QPushButton* b : dlg.findChildren<QPushButton*>()) {
                if (b->property("btnSize").toString() == QLatin1String("dialog") &&
                    b->text() != QLatin1String("Close")) {
                    QVERIFY2(!b->isEnabled(),
                             qPrintable(QStringLiteral("SessionManager button '%1' stayed enabled after clearSelection()")
                                            .arg(b->text())));
                }
            }

            // Light mode unfocused selected parent row (:selected:!active) branch chevron contrast regression
            if (!dark) {
                QTreeWidgetItem* grpItem = tree->topLevelItem(0);
                tree->setCurrentItem(grpItem);
                searchEdit->setFocus();
                QCoreApplication::processEvents();
                QTest::qWait(30);

                const QImage vpImg = tree->viewport()->grab().toImage().convertToFormat(QImage::Format_RGBA8888);
                const qreal dpr = (vpImg.devicePixelRatio() > 0.0) ? vpImg.devicePixelRatio() : 1.0;
                const QRect itemRect = tree->visualItemRect(grpItem);
                const int bx1 = std::min(vpImg.width(), static_cast<int>(std::lround(std::max(16, tree->indentation()) * dpr)));
                const int by0 = std::max(0, static_cast<int>(std::lround(itemRect.top() * dpr)));
                const int by1 = std::min(vpImg.height(), static_cast<int>(std::lround((itemRect.bottom() + 1) * dpr)));
                int whitePx = 0;
                int darkSlatePx = 0;
                for (int y = by0; y < by1; ++y) {
                    for (int x = 0; x < bx1; ++x) {
                        const QColor c = vpImg.pixelColor(x, y);
                        if (c.red() > 245 && c.green() > 245 && c.blue() > 245) ++whitePx;
                        if (c.red() < 160 && c.green() < 175 && c.blue() < 195) ++darkSlatePx;
                    }
                }
                QVERIFY2(whitePx == 0,
                         qPrintable(QStringLiteral("Light unfocused parent branch drew %1 white pixels on #dbeafe")
                                        .arg(whitePx)));
                QVERIFY2(darkSlatePx > 0,
                         "Light unfocused parent branch missing dark slate chevron pixels");
            }

            // Restore host selection for Shot 04
            tree->setCurrentItem(tree->topLevelItem(0)->child(0));
            QCoreApplication::processEvents();
            QVERIFY(primaryBtn->isEnabled());

            int dialogBtnRefH = -1;
            int topBtnRefH = searchEdit->height();
            for (QPushButton* b : dlg.findChildren<QPushButton*>()) {
                QVERIFY2(b->geometry().right() <= dlg.width(),
                         qPrintable(QStringLiteral("SessionManager button '%1' overflows dialog width %2 > %3")
                                        .arg(b->text()).arg(b->geometry().right()).arg(dlg.width())));
                if (b->property("btnSize").toString() == QLatin1String("dialog")) {
                    if (dialogBtnRefH < 0) dialogBtnRefH = b->height();
                    QVERIFY2(std::abs(b->height() - dialogBtnRefH) <= 1,
                             qPrintable(QStringLiteral("SessionManager footer button '%1' height %2 != %3")
                                            .arg(b->text()).arg(b->height()).arg(dialogBtnRefH)));
                } else {
                    QVERIFY2(std::abs(b->height() - topBtnRefH) <= 2,
                             qPrintable(QStringLiteral("SessionManager top button '%1' height %2 != search %3")
                                            .arg(b->text()).arg(b->height()).arg(topBtnRefH)));
                }
            }

            saveShot(dlg.grab(), QStringLiteral("04_session_manager_%1.png").arg(tag));
        }

        // -------------------------------------------------------------------
        // 3. SessionEditDialog (ALL 3 Tabs, 5 QComboBoxes, 3 QSpinBoxes, #keyBrowseBtn, QCompleter, X-align)
        // -------------------------------------------------------------------
        {
            SessionProfile prof = SessionProfile::createNew();
            prof.name = QStringLiteral("GPU-Node-01");
            prof.group = QStringLiteral("GPU Cluster");
            prof.host = QStringLiteral("10.0.10.21");
            prof.port = 22;
            prof.username = QStringLiteral("mlops");
            prof.authType = AuthType::Password;

            SessionEditDialog dlg(prof, store.groups());
            dlg.show();
            QCoreApplication::processEvents();
            QTest::qWait(40);

            auto* tabs = dlg.findChild<QTabWidget*>();
            QVERIFY(tabs != nullptr && tabs->count() == 3);

            // Tab 0: Connection
            tabs->setCurrentIndex(0);
            QCoreApplication::processEvents();
            QTest::qWait(30);

            auto* keyBrowseBtn = dlg.findChild<QPushButton*>(QStringLiteral("keyBrowseBtn"));
            QVERIFY(keyBrowseBtn != nullptr);
            QVERIFY(keyBrowseBtn->isEnabled());  // Protected contract under AuthType::Password

            auto* groupCombo = dlg.findChild<QComboBox*>(QStringLiteral("groupCombo"));
            auto* authCombo = dlg.findChild<QComboBox*>(QStringLiteral("authCombo"));
            QVERIFY(groupCombo && groupCombo->isEditable() && groupCombo->lineEdit());
            QVERIFY(authCombo != nullptr);
            verifyComboGeometry(groupCombo, true);
            verifyComboGeometry(authCombo, true);
            const int tab0InputX = groupCombo->mapTo(tabs, QPoint(0, 0)).x();

            // Verify QCompleter popup on editable groupCombo_ is installed, styled, and themed
            {
                QCompleter* comp = groupCombo->completer();
                QVERIFY(comp != nullptr);
                QCOMPARE(comp->completionMode(), QCompleter::PopupCompletion);
                QAbstractItemView* compPopup = comp->popup();
                QVERIFY(compPopup != nullptr);
                QCOMPARE(compPopup->objectName(), QStringLiteral("comboPopupView"));
                QVERIFY(qobject_cast<QStyledItemDelegate*>(compPopup->itemDelegate()) != nullptr);

                comp->setCompletionPrefix(QStringLiteral("GPU"));
                comp->complete();
                QCoreApplication::processEvents();
                QTest::qWait(40);
                QVERIFY(compPopup->isVisible());
                if (compPopup->model() && compPopup->model()->rowCount() > 0) {
                    const QRect r0 = compPopup->visualRect(compPopup->model()->index(0, 0));
                    QVERIFY2(r0.height() >= 22,
                             qPrintable(QStringLiteral("QCompleter popup row0 height %1 < 22").arg(r0.height())));
                }
                const QImage compImg = compPopup->grab().toImage().convertToFormat(QImage::Format_RGBA8888);
                const LuminanceStats cst = analyzeInteriorLuminance(compImg, 3);
                if (dark) {
                    QVERIFY2(cst.medianY < 70.0 && cst.nearWhiteFraction < 0.05,
                             qPrintable(QStringLiteral("Dark QCompleter popup unstyled (medianY=%1 nearWhite=%2)")
                                            .arg(cst.medianY).arg(cst.nearWhiteFraction)));
                } else {
                    QVERIFY2(cst.medianY > 210.0 && cst.minY < 160.0,
                             qPrintable(QStringLiteral("Light QCompleter popup unstyled (medianY=%1 minY=%2)")
                                            .arg(cst.medianY).arg(cst.minY)));
                }
                compPopup->hide();
                comp->setCompletionPrefix(QString());
                QCoreApplication::processEvents();
                QTest::qWait(40);
                dlg.activateWindow();
                QCoreApplication::processEvents();
            }

            // Inner QLineEdit of editable groupCombo_ must sit strictly inside combo rect
            const QRect innerRect = groupCombo->lineEdit()->geometry();
            QVERIFY2(groupCombo->rect().contains(innerRect),
                     qPrintable(QStringLiteral("Editable groupCombo inner QLineEdit (%1,%2 %3x%4) outside rect (%5x%6)")
                                    .arg(innerRect.x()).arg(innerRect.y()).arg(innerRect.width()).arg(innerRect.height())
                                    .arg(groupCombo->width()).arg(groupCombo->height())));

            QWidget* tab0 = tabs->widget(0);
            const int refControlH = groupCombo->height();
            for (QLineEdit* le : tab0->findChildren<QLineEdit*>()) {
                if (le->parentWidget() == groupCombo || qobject_cast<QSpinBox*>(le->parentWidget())) continue;
                QVERIFY2(std::abs(le->height() - refControlH) <= 2,
                         qPrintable(QStringLiteral("Tab0 QLineEdit height %1 != %2").arg(le->height()).arg(refControlH)));
            }
            for (QSpinBox* sp : tab0->findChildren<QSpinBox*>()) {
                QVERIFY2(std::abs(sp->height() - refControlH) <= 2, "Tab0 QSpinBox height mismatch");
                verifySpinBoxFitsMax(sp, 65535, "portSpin");
            }
            QVERIFY2(std::abs(authCombo->height() - refControlH) <= 2, "Tab0 authCombo height mismatch");
            QVERIFY2(std::abs(keyBrowseBtn->height() - refControlH) <= 2, "Tab0 keyBrowseBtn height mismatch");

            saveShot(dlg.grab(), QStringLiteral("05_session_edit_tab0_conn_%1.png").arg(tag));

            // Combo #4 (editable groupCombo_) + functional edit check + Shot 06
            verifyComboPopup(groupCombo, &dlg, dark,
                             QStringLiteral("06_session_edit_editable_combo_open_%1.png").arg(tag),
                             QStringLiteral("06_session_edit_editable_combo_popup_only_%1.png").arg(tag));
            groupCombo->setEditText(QStringLiteral("CustomGroup"));
            QCOMPARE(groupCombo->currentText(), QStringLiteral("CustomGroup"));

            // Combo #5 (authCombo_)
            verifyComboPopup(authCombo, &dlg, dark);

            // Footer buttons check (testBtn_ + QDialogButtonBox Ok/Cancel)
            auto* btnBox = dlg.findChild<QDialogButtonBox*>();
            QVERIFY(btnBox != nullptr);
            QPushButton* okBtn = btnBox->button(QDialogButtonBox::Ok);
            QPushButton* cancelBtn = btnBox->button(QDialogButtonBox::Cancel);
            QVERIFY(okBtn && cancelBtn);
            QCOMPARE(okBtn->objectName(), QStringLiteral("primaryBtn"));
            QVERIFY(std::abs(okBtn->height() - cancelBtn->height()) <= 1);

            // Tab 1: Protocols (Combos #6 & #7 + keepAliveSpin_ + Shot 07)
            tabs->setCurrentIndex(1);
            QCoreApplication::processEvents();
            QTest::qWait(30);

            auto* protoCombo = dlg.findChild<QComboBox*>(QStringLiteral("protoCombo"));
            auto* r2rCombo = dlg.findChild<QComboBox*>(QStringLiteral("r2rCombo"));
            auto* keepAliveSpin = tabs->widget(1)->findChild<QSpinBox*>();
            QVERIFY(protoCombo && r2rCombo && keepAliveSpin);
            QVERIFY(std::abs(protoCombo->height() - r2rCombo->height()) <= 1);
            QVERIFY(std::abs(keepAliveSpin->height() - protoCombo->height()) <= 2);
            verifyComboGeometry(protoCombo, true);
            verifyComboGeometry(r2rCombo, true);
            verifySpinBoxFitsMax(keepAliveSpin, 300, "keepAliveSpin");
            const int tab1InputX = protoCombo->mapTo(tabs, QPoint(0, 0)).x();
            QVERIFY2(std::abs(tab1InputX - tab0InputX) <= 1,
                     qPrintable(QStringLiteral("SessionEditDialog Tab1 input X=%1 != Tab0 X=%2")
                                    .arg(tab1InputX).arg(tab0InputX)));

            verifyComboPopup(protoCombo, &dlg, dark,
                             QStringLiteral("07_session_edit_tab1_proto_%1.png").arg(tag),
                             QStringLiteral("07_session_edit_tab1_proto_popup_only_%1.png").arg(tag));
            verifyComboPopup(r2rCombo, &dlg, dark);

            // Tab 2: JumpHost (Disabled Shot 08 + Enabled + Combo #8 Shot 09)
            tabs->setCurrentIndex(2);
            QCoreApplication::processEvents();
            QTest::qWait(30);

            auto* useJumpCheck = tabs->widget(2)->findChild<QCheckBox*>();
            auto* jumpContainer = dlg.findChild<QWidget*>(QStringLiteral("jumpContainer"));
            auto* jumpAuthCombo = dlg.findChild<QComboBox*>(QStringLiteral("jumpAuthCombo"));
            auto* jumpSpin = tabs->widget(2)->findChild<QSpinBox*>();
            QVERIFY(useJumpCheck && jumpContainer && jumpAuthCombo && jumpSpin);
            QVERIFY(!useJumpCheck->isChecked());
            QVERIFY(!jumpContainer->isEnabled());

            // Verify zero double-nested horizontal margin & cross-tab left-X alignment on Tab 2
            QVERIFY2(jumpContainer->x() <= 16,
                     qPrintable(QStringLiteral("Tab 2 jumpContainer left offset %1 > 16 (double-nested margin!)")
                                    .arg(jumpContainer->x())));
            const int tab2InputX = jumpAuthCombo->mapTo(tabs, QPoint(0, 0)).x();
            QVERIFY2(std::abs(tab2InputX - tab0InputX) <= 1,
                     qPrintable(QStringLiteral("SessionEditDialog Tab2 input X=%1 != Tab0 X=%2")
                                    .arg(tab2InputX).arg(tab0InputX)));

            saveShot(dlg.grab(), QStringLiteral("08_session_edit_tab2_jump_disabled_%1.png").arg(tag));

            useJumpCheck->setChecked(true);
            QCoreApplication::processEvents();
            QTest::qWait(30);
            QVERIFY(jumpContainer->isEnabled());
            QVERIFY(std::abs(jumpAuthCombo->height() - jumpSpin->height()) <= 2);
            verifyComboGeometry(jumpAuthCombo, true);
            verifySpinBoxFitsMax(jumpSpin, 65535, "jumpPortSpin");

            verifyComboPopup(jumpAuthCombo, &dlg, dark,
                             QStringLiteral("09_session_edit_tab2_jump_enabled_%1.png").arg(tag),
                             QStringLiteral("09_session_edit_tab2_jump_enabled_popup_only_%1.png").arg(tag));
        }

        // -------------------------------------------------------------------
        // 4. BookmarkManagerDialog (Shot 10 + clearSelection) & BookmarkEditDialog (Shot 11)
        // -------------------------------------------------------------------
        {
            BookmarkManagerDialog dlg(&store, EndpointSpec::local(), workDir);
            dlg.show();
            QCoreApplication::processEvents();
            QTest::qWait(40);

            auto* upBtn = dlg.findChild<QToolButton*>(QStringLiteral("moveUpBtn"));
            auto* downBtn = dlg.findChild<QToolButton*>(QStringLiteral("moveDownBtn"));
            auto* openBtn = dlg.findChild<QPushButton*>(QStringLiteral("primaryBtn"));
            auto* table = dlg.findChild<QTableWidget*>();
            QVERIFY(upBtn && downBtn && openBtn && table);
            QVERIFY(table->rowCount() == 3);
            QVERIFY(!openBtn->isEnabled());  // No row selected initially

            // Select middle row -> clearSelection() -> ALL selection-dependent controls MUST disable
            table->selectRow(1);
            QCoreApplication::processEvents();
            QVERIFY(upBtn->isEnabled() && downBtn->isEnabled() && openBtn->isEnabled());
            table->clearSelection();
            QCoreApplication::processEvents();
            QVERIFY(!upBtn->isEnabled());
            QVERIFY(!downBtn->isEnabled());
            QVERIFY(!openBtn->isEnabled());
            for (QPushButton* b : dlg.findChildren<QPushButton*>()) {
                if (b->text() == QLatin1String("Edit") || b->text() == QLatin1String("Delete")) {
                    QVERIFY2(!b->isEnabled(),
                             qPrintable(QStringLiteral("BookmarkManager button '%1' stayed enabled after clearSelection()")
                                            .arg(b->text())));
                }
            }

            // Select row 0 -> upBtn disabled, downBtn & openBtn enabled
            table->selectRow(0);
            QCoreApplication::processEvents();
            QVERIFY(!upBtn->isEnabled());
            QVERIFY(downBtn->isEnabled());
            QVERIFY(openBtn->isEnabled());

            // Square 28x28 reorder buttons
            QVERIFY(std::abs(upBtn->width() - upBtn->height()) <= 1);
            QVERIFY(std::abs(downBtn->width() - downBtn->height()) <= 1);
            QVERIFY(std::abs(upBtn->height() - downBtn->height()) <= 1);

            // Zero overflow + uniform tier heights across top toolbar & bottom footer
            int footerH = -1;
            for (QPushButton* b : dlg.findChildren<QPushButton*>()) {
                QVERIFY2(b->geometry().right() <= dlg.width(),
                         qPrintable(QStringLiteral("BookmarkManager button '%1' overflows dialog width %2 > %3")
                                        .arg(b->text()).arg(b->geometry().right()).arg(dlg.width())));
                if (b->property("btnSize").toString() == QLatin1String("dialog")) {
                    if (footerH < 0) footerH = b->height();
                    QVERIFY2(std::abs(b->height() - footerH) <= 1, "BookmarkManager footer height mismatch");
                } else {
                    QVERIFY2(std::abs(b->height() - upBtn->height()) <= 1,
                             qPrintable(QStringLiteral("BookmarkManager top button '%1' height %2 != %3")
                                            .arg(b->text()).arg(b->height()).arg(upBtn->height())));
                }
            }

            saveShot(dlg.grab(), QStringLiteral("10_bookmark_manager_%1.png").arg(tag));
        }

        {
            BookmarkEditDialog editDlg(QStringLiteral("Edit Path Bookmark"),
                                       QStringLiteral("Checkpoints"),
                                       QStringLiteral("/data/models/checkpoints"));
            editDlg.show();
            QCoreApplication::processEvents();
            QTest::qWait(30);

            const auto edits = editDlg.findChildren<QLineEdit*>();
            QCOMPARE(edits.size(), 2);
            QVERIFY(std::abs(edits[0]->height() - edits[1]->height()) <= 1);

            auto* box = editDlg.findChild<QDialogButtonBox*>();
            QVERIFY(box != nullptr);
            QPushButton* okBtn = box->button(QDialogButtonBox::Ok);
            QPushButton* cancelBtn = box->button(QDialogButtonBox::Cancel);
            QVERIFY(okBtn && cancelBtn);
            QCOMPARE(okBtn->objectName(), QStringLiteral("primaryBtn"));
            QVERIFY(std::abs(okBtn->height() - cancelBtn->height()) <= 1);
            QVERIFY(box->geometry().bottom() <= editDlg.height());

            saveShot(editDlg.grab(), QStringLiteral("11_bookmark_edit_dialog_%1.png").arg(tag));
        }

        transferMgr.shutdown();
    }

    std::unique_ptr<SshGlobalInit> sshInit_;

private slots:
    void initTestCase() {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        sshInit_ = std::make_unique<SshGlobalInit>();
        QStandardPaths::setTestModeEnabled(true);
        ThemeManager::initialize(*qApp);
        ThemeManager::applyTheme(true);
    }

    void cleanupTestCase() {
        sshInit_.reset();
    }

    void testStyleHintsAndIconAssets() {
        QVERIFY(qApp->style() != nullptr);
        QCOMPARE(qApp->style()->styleHint(QStyle::SH_ComboBox_Popup), 0);
        QCOMPARE(qApp->style()->styleHint(QStyle::SH_DialogButtonBox_ButtonsHaveIcons), 0);
        QCOMPARE(qApp->style()->styleHint(QStyle::SH_EtchDisabledText), 0);
        QCOMPARE(qApp->style()->styleHint(QStyle::SH_ScrollBar_Transient), 0);

        const QStringList uiSvgs = {
            QStringLiteral("chevron_down_dark.svg"),
            QStringLiteral("chevron_down_light.svg"),
            QStringLiteral("chevron_down_disabled_dark.svg"),
            QStringLiteral("chevron_down_disabled_light.svg"),
            QStringLiteral("chevron_down_white.svg"),
            QStringLiteral("chevron_up_dark.svg"),
            QStringLiteral("chevron_up_light.svg"),
            QStringLiteral("chevron_up_disabled_dark.svg"),
            QStringLiteral("chevron_up_disabled_light.svg"),
            QStringLiteral("chevron_right_dark.svg"),
            QStringLiteral("chevron_right_light.svg"),
            QStringLiteral("chevron_right_white.svg"),
            QStringLiteral("check_white.svg"),
            QStringLiteral("check_disabled_dark.svg"),
            QStringLiteral("check_disabled_light.svg"),
            QStringLiteral("close_dark.svg"),
            QStringLiteral("close_dark_hover.svg"),
            QStringLiteral("close_light.svg"),
            QStringLiteral("close_light_hover.svg"),
            QStringLiteral("arrow_left.svg"),
            QStringLiteral("arrow_up.svg"),
            QStringLiteral("arrow_down.svg"),
            QStringLiteral("home.svg"),
            QStringLiteral("refresh.svg"),
            QStringLiteral("transfer.svg"),
            QStringLiteral("window_new.svg"),
            QStringLiteral("plus.svg"),
            QStringLiteral("star_outline.svg"),
            QStringLiteral("star_filled.svg"),
            QStringLiteral("bookmarks.svg"),
        };
        for (const QString& name : uiSvgs) {
            QFile f(QStringLiteral(":/icons/ui/") + name);
            QVERIFY2(f.exists() && f.open(QIODevice::ReadOnly) && !f.readAll().isEmpty(),
                     qPrintable(QStringLiteral("Missing or empty SVG resource: %1").arg(name)));
        }

        // Verify all 11 themed icons render non-null and recolor on Dark <-> Light switch
        ThemeManager::applyTheme(true);
        const QImage darkArrow = IconCache::arrowLeftIcon().pixmap(QSize(16, 16)).toImage();
        QVERIFY(!darkArrow.isNull());
        ThemeManager::applyTheme(false);
        const QImage lightArrow = IconCache::arrowLeftIcon().pixmap(QSize(16, 16)).toImage();
        QVERIFY(!lightArrow.isNull());
        QVERIFY2(darkArrow != lightArrow, "ThemedSvgIconEngine did not recolor icon between Dark and Light modes");

        QVERIFY(!IconCache::arrowUpIcon().isNull());
        QVERIFY(!IconCache::arrowDownIcon().isNull());
        QVERIFY(!IconCache::homeIcon().isNull());
        QVERIFY(!IconCache::refreshIcon().isNull());
        QVERIFY(!IconCache::transferIcon().isNull());
        QVERIFY(!IconCache::newWindowIcon().isNull());
        QVERIFY(!IconCache::plusIcon().isNull());
        QVERIFY(!IconCache::starOutlineIcon().isNull());
        QVERIFY(!IconCache::starFilledIcon().isNull());
        QVERIFY(!IconCache::bookmarksIcon().isNull());
    }

    void testDarkThemeUiAndPopups() {
        runFullSuiteForTheme(true);
    }

    void testLightThemeUiAndPopups() {
        runFullSuiteForTheme(false);
    }

    void testLiveThemeToggleMultiWindowSync() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        SessionStore store(tmp.filePath(QStringLiteral("sessions.json")));
        TransferManager mgr;

        ThemeManager::applyTheme(true);
        std::unique_ptr<MainWindow> w1(new MainWindow(&store, &mgr, {}));
        std::unique_ptr<MainWindow> w2(new MainWindow(&store, &mgr, {}));
        w1->setAttribute(Qt::WA_DeleteOnClose, false);
        w2->setAttribute(Qt::WA_DeleteOnClose, false);
        w1->show();
        w2->show();
        QCoreApplication::processEvents();

        auto findDarkAct = [](MainWindow* w) -> QAction* {
            for (QAction* a : w->findChildren<QAction*>()) {
                if (a->text() == QLatin1String("Dark Theme")) return a;
            }
            return nullptr;
        };
        QAction* act1 = findDarkAct(w1.get());
        QAction* act2 = findDarkAct(w2.get());
        QVERIFY(act1 && act2);
        QVERIFY(act1->isChecked() && act2->isChecked());

        // Live toggle Dark -> Light from w1
        w1->applyTheme(false);
        QCoreApplication::processEvents();
        QVERIFY(!ThemeManager::isDark());
        QVERIFY(!IconCache::isDarkTheme());
        QVERIFY(!act1->isChecked());
        QVERIFY(!act2->isChecked());
        QVERIFY(qApp->palette().color(QPalette::Window).lightness() > 200);
        QVERIFY(qApp->palette().color(QPalette::ToolTipBase).lightness() > 220);

        // Live toggle Light -> Dark from w2
        w2->applyTheme(true);
        QCoreApplication::processEvents();
        QVERIFY(ThemeManager::isDark());
        QVERIFY(IconCache::isDarkTheme());
        QVERIFY(act1->isChecked());
        QVERIFY(act2->isChecked());
        QVERIFY(qApp->palette().color(QPalette::Window).lightness() < 50);
        QVERIFY(qApp->palette().color(QPalette::ToolTipBase).lightness() < 60);

        w1->close();
        w2->close();
        mgr.shutdown();
    }

    void testWcagContrastAndProgressDelegate() {
        auto srgbToLinear = [](int ch) -> double {
            const double c = static_cast<double>(ch) / 255.0;
            return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
        };
        auto relLum = [&](const QColor& c) -> double {
            return 0.2126 * srgbToLinear(c.red()) + 0.7152 * srgbToLinear(c.green()) +
                   0.0722 * srgbToLinear(c.blue());
        };
        auto contrastRatio = [&](const QColor& fg, const QColor& bg) -> double {
            const double l1 = relLum(fg);
            const double l2 = relLum(bg);
            const double hi = std::max(l1, l2);
            const double lo = std::min(l1, l2);
            return (hi + 0.05) / (lo + 0.05);
        };
        auto assertContrast = [&](const QColor& fg, const QColor& bg, const char* label) {
            const double ratio = contrastRatio(fg, bg);
            QVERIFY2(ratio >= 4.5,
                     qPrintable(QStringLiteral("WCAG AA fail for %1 (%2 on %3): ratio=%4 < 4.5")
                                    .arg(QString::fromLatin1(label), fg.name(), bg.name())
                                    .arg(ratio, 0, 'f', 2)));
        };

        const TransferProtocol allProtos[] = {
            TransferProtocol::Rsync,
            TransferProtocol::RemoteDirectRsync,
            TransferProtocol::Sftp,
            TransferProtocol::RemoteRelaySftp,
            TransferProtocol::Scp,
        };
        const TransferStatus allStatuses[] = {
            TransferStatus::Failed,
            TransferStatus::Completed,
            TransferStatus::Paused,
            TransferStatus::Canceled,
            TransferStatus::Scanning,
        };

        for (bool dark : {true, false}) {
            ThemeManager::applyTheme(dark);
            const QColor baseBg(dark ? QStringLiteral("#141821") : QStringLiteral("#ffffff"));
            const QColor altBg(dark ? QStringLiteral("#181d28") : QStringLiteral("#f8fafc"));
            const QColor dlgBg(dark ? QStringLiteral("#161922") : QStringLiteral("#f4f6fa"));

            for (TransferProtocol p : allProtos) {
                const QColor c = ThemeManager::protocolColor(p);
                assertContrast(c, baseBg, dark ? "dark protocol on base" : "light protocol on base");
                assertContrast(c, altBg, dark ? "dark protocol on alt" : "light protocol on alt");
            }
            for (TransferStatus st : allStatuses) {
                const QColor c = ThemeManager::statusColor(st);
                assertContrast(c, baseBg, dark ? "dark status on base" : "light status on base");
                assertContrast(c, altBg, dark ? "dark status on alt" : "light status on alt");
            }
            assertContrast(ThemeManager::errorTextColor(), baseBg, dark ? "dark error base" : "light error base");
            assertContrast(ThemeManager::errorTextColor(), dlgBg, dark ? "dark error dlg" : "light error dlg");

            const ProgressBarColors pc = ThemeManager::progressColors();
            assertContrast(pc.textOnFill, pc.fillRunning, "progress textOnFill vs fillRunning");
            assertContrast(pc.textOnFill, pc.fillCompleted, "progress textOnFill vs fillCompleted");
            assertContrast(pc.textOnFill, pc.fillFailed, "progress textOnFill vs fillFailed");
            assertContrast(pc.textOnFill, pc.fillPaused, "progress textOnFill vs fillPaused");
            assertContrast(pc.textOnTrack, pc.track, "progress textOnTrack vs track");

            const DragBadgeColors dbc = ThemeManager::dragBadgeColors();
            QColor opaqueBadgeBg(dbc.bg.red(), dbc.bg.green(), dbc.bg.blue());
            assertContrast(dbc.text, opaqueBadgeBg, "drag badge text vs bg");

            // Render ProgressBarDelegate at 100% + Paused (must draw amber fillPaused, NOT green!)
            // and at 100% + Failed (must draw red fillFailed, NOT green!).
            QStandardItemModel m(2, 1);
            auto* pausedItem = new QStandardItem();
            pausedItem->setData(100, TransferQueueModel::RoleProgressPercent);
            pausedItem->setData(static_cast<int>(TransferStatus::Paused), TransferQueueModel::RoleStatusInt);
            m.setItem(0, 0, pausedItem);

            auto* failedItem = new QStandardItem();
            failedItem->setData(100, TransferQueueModel::RoleProgressPercent);
            failedItem->setData(static_cast<int>(TransferStatus::Failed), TransferQueueModel::RoleStatusInt);
            m.setItem(1, 0, failedItem);

            ProgressBarDelegate delegate;
            auto renderRow = [&](int row) -> QImage {
                QImage img(140, 28, QImage::Format_RGBA8888);
                img.fill(baseBg);
                QPainter p(&img);
                QStyleOptionViewItem opt;
                opt.rect = QRect(0, 0, 140, 28);
                opt.state = QStyle::State_Enabled;
                delegate.paint(&p, opt, m.index(row, 0));
                return img;
            };
            auto colorNear = [](const QColor& a, const QColor& b) {
                return std::abs(a.red() - b.red()) <= 12 &&
                       std::abs(a.green() - b.green()) <= 12 &&
                       std::abs(a.blue() - b.blue()) <= 12;
            };

            const QImage pausedImg = renderRow(0);
            const QColor pausedPx = pausedImg.pixelColor(10, 14);
            QVERIFY2(colorNear(pausedPx, pc.fillPaused),
                     qPrintable(QStringLiteral("100%% Paused bar drew %1 instead of fillPaused %2")
                                    .arg(pausedPx.name(), pc.fillPaused.name())));
            QVERIFY2(!colorNear(pausedPx, pc.fillCompleted),
                     "100% Paused bar mistakenly drew green fillCompleted");

            const QImage failedImg = renderRow(1);
            const QColor failedPx = failedImg.pixelColor(10, 14);
            QVERIFY2(colorNear(failedPx, pc.fillFailed),
                     qPrintable(QStringLiteral("100%% Failed bar drew %1 instead of fillFailed %2")
                                    .arg(failedPx.name(), pc.fillFailed.name())));
        }

        // Parse live :/styles/light.qss rules directly (no duplicated hex literals)
        // and verify WCAG 2.1 AA (>= 4.5:1) across QWidget, #paneFooter, status labels, and danger buttons.
        QFile lightQssFile(QStringLiteral(":/styles/light.qss"));
        QVERIFY2(lightQssFile.open(QIODevice::ReadOnly | QIODevice::Text),
                 "Failed to open embedded :/styles/light.qss");
        QString lightQss = QString::fromUtf8(lightQssFile.readAll());
        QVERIFY2(!lightQss.isEmpty(), ":/styles/light.qss is empty");

        lightQss.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                           QRegularExpression::DotMatchesEverythingOption));

        QMap<QString, QMap<QString, QString>> qssRules;
        const QRegularExpression blockRe(QStringLiteral("([^{}]+)\\{([^{}]*)\\}"));
        QRegularExpressionMatchIterator it = blockRe.globalMatch(lightQss);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QStringList selectors = m.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts);
            const QStringList decls = m.captured(2).split(QLatin1Char(';'), Qt::SkipEmptyParts);
            QMap<QString, QString> props;
            for (const QString& decl : decls) {
                const int colon = decl.indexOf(QLatin1Char(':'));
                if (colon <= 0) continue;
                const QString key = decl.left(colon).trimmed();
                const QString val = decl.mid(colon + 1).trimmed();
                if (!key.isEmpty() && !val.isEmpty()) {
                    props.insert(key, val);
                }
            }
            for (const QString& rawSel : selectors) {
                const QString sel = rawSel.simplified();
                if (sel.isEmpty()) continue;
                auto& dst = qssRules[sel];
                for (auto pit = props.constBegin(); pit != props.constEnd(); ++pit) {
                    dst.insert(pit.key(), pit.value());
                }
            }
        }

        const QRegularExpression hexColorRe(QStringLiteral("^#[0-9a-fA-F]{6}$"));
        auto extractQssColor = [&](const QString& selector, const QString& prop, QString& errOut) -> QColor {
            const auto sit = qssRules.constFind(selector);
            if (sit == qssRules.constEnd()) {
                errOut = QStringLiteral("light.qss missing selector '%1'").arg(selector);
                return {};
            }
            const auto pit = sit.value().constFind(prop);
            if (pit == sit.value().constEnd()) {
                errOut = QStringLiteral("light.qss selector '%1' missing property '%2'").arg(selector, prop);
                return {};
            }
            const QString val = pit.value();
            if (!hexColorRe.match(val).hasMatch()) {
                errOut = QStringLiteral("light.qss %1 { %2: %3 } is not a 6-digit hex color")
                             .arg(selector, prop, val);
                return {};
            }
            const QColor c(val);
            if (!c.isValid()) {
                errOut = QStringLiteral("light.qss %1 { %2: %3 } invalid QColor").arg(selector, prop, val);
                return {};
            }
            return c;
        };

        QString parseErr;
        const QColor lightDlgBg = extractQssColor(QStringLiteral("QDialog"), QStringLiteral("background-color"), parseErr);
        QVERIFY2(lightDlgBg.isValid(), qPrintable(parseErr));
        const QColor lightWidgetFg = extractQssColor(QStringLiteral("QWidget"), QStringLiteral("color"), parseErr);
        QVERIFY2(lightWidgetFg.isValid(), qPrintable(parseErr));
        const QColor lightHeaderBg = extractQssColor(QStringLiteral("QFrame#paneHeader"), QStringLiteral("background-color"), parseErr);
        QVERIFY2(lightHeaderBg.isValid(), qPrintable(parseErr));
        const QColor lightFooterBg = extractQssColor(QStringLiteral("QLabel#paneFooter"), QStringLiteral("background-color"), parseErr);
        QVERIFY2(lightFooterBg.isValid(), qPrintable(parseErr));
        const QColor lightFooterFg = extractQssColor(QStringLiteral("QLabel#paneFooter"), QStringLiteral("color"), parseErr);
        QVERIFY2(lightFooterFg.isValid(), qPrintable(parseErr));
        const QColor lightMutedFg = extractQssColor(QStringLiteral("QLabel[status=\"muted\"]"), QStringLiteral("color"), parseErr);
        QVERIFY2(lightMutedFg.isValid(), qPrintable(parseErr));
        const QColor lightOkFg = extractQssColor(QStringLiteral("QLabel[status=\"ok\"]"), QStringLiteral("color"), parseErr);
        QVERIFY2(lightOkFg.isValid(), qPrintable(parseErr));
        const QColor lightWarnFg = extractQssColor(QStringLiteral("QLabel[status=\"warn\"]"), QStringLiteral("color"), parseErr);
        QVERIFY2(lightWarnFg.isValid(), qPrintable(parseErr));
        const QColor lightErrorFg = extractQssColor(QStringLiteral("QLabel[status=\"error\"]"), QStringLiteral("color"), parseErr);
        QVERIFY2(lightErrorFg.isValid(), qPrintable(parseErr));
        const QColor lightDangerBg = extractQssColor(QStringLiteral("QPushButton[variant=\"danger\"]"), QStringLiteral("background-color"), parseErr);
        QVERIFY2(lightDangerBg.isValid(), qPrintable(parseErr));
        const QColor lightDangerFg = extractQssColor(QStringLiteral("QPushButton[variant=\"danger\"]"), QStringLiteral("color"), parseErr);
        QVERIFY2(lightDangerFg.isValid(), qPrintable(parseErr));
        const QColor lightDangerHoverBg = extractQssColor(QStringLiteral("QPushButton[variant=\"danger\"]:hover"), QStringLiteral("background-color"), parseErr);
        QVERIFY2(lightDangerHoverBg.isValid(), qPrintable(parseErr));
        const QColor lightDangerHoverFg = extractQssColor(QStringLiteral("QPushButton[variant=\"danger\"]:hover"), QStringLiteral("color"), parseErr);
        QVERIFY2(lightDangerHoverFg.isValid(), qPrintable(parseErr));
        const QColor lightDangerPressedBg = extractQssColor(QStringLiteral("QPushButton[variant=\"danger\"]:pressed"), QStringLiteral("background-color"), parseErr);
        QVERIFY2(lightDangerPressedBg.isValid(), qPrintable(parseErr));
        const QColor lightDangerPressedFg = extractQssColor(QStringLiteral("QPushButton[variant=\"danger\"]:pressed"), QStringLiteral("color"), parseErr);
        QVERIFY2(lightDangerPressedFg.isValid(), qPrintable(parseErr));

        assertContrast(lightWidgetFg, lightDlgBg, "light.qss QWidget color on QDialog");
        assertContrast(lightFooterFg, lightFooterBg, "light.qss QLabel#paneFooter");
        assertContrast(lightMutedFg, lightDlgBg, "light.qss status=muted on QDialog");
        assertContrast(lightMutedFg, lightHeaderBg, "light.qss status=muted on #paneHeader");
        assertContrast(lightOkFg, lightDlgBg, "light.qss status=ok on QDialog");
        assertContrast(lightOkFg, lightHeaderBg, "light.qss status=ok on #paneHeader");
        assertContrast(lightWarnFg, lightDlgBg, "light.qss status=warn on QDialog");
        assertContrast(lightWarnFg, lightHeaderBg, "light.qss status=warn on #paneHeader");
        assertContrast(lightErrorFg, lightDlgBg, "light.qss status=error on QDialog");
        assertContrast(lightErrorFg, lightHeaderBg, "light.qss status=error on #paneHeader");
        assertContrast(lightDangerFg, lightDangerBg, "light.qss variant=danger");
        assertContrast(lightDangerHoverFg, lightDangerHoverBg, "light.qss variant=danger:hover");
        assertContrast(lightDangerPressedFg, lightDangerPressedBg, "light.qss variant=danger:pressed");
    }

    void testInitialRightSessionLongHostnameSplit() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        SessionStore store(tmp.filePath(QStringLiteral("sessions.json")));
        TransferManager transferMgr;

        SessionProfile longRemote = SessionProfile::createNew();
        longRemote.name = QStringLiteral("gpu-cluster-prod-very-long-hostname-01.internal.example.org");
        longRemote.host = QStringLiteral("gpu-cluster-prod-very-long-hostname-01.internal.example.org");
        longRemote.port = 1;  // Immediate refusal; verifies layout without live SSH server
        longRemote.username = QStringLiteral("mlops");

        SessionProfile shortRemote = SessionProfile::createNew();
        shortRemote.name = QStringLiteral("GPU-01");
        shortRemote.host = QStringLiteral("127.0.0.1");
        shortRemote.port = 1;
        shortRemote.username = QStringLiteral("mlops");

        auto verifySplitWindow = [&](MainWindow* win, const QString& expectedName,
                                     bool expectElided, const QString& stage) {
            FilePane* leftPane = win->leftTabs()->currentFilePane();
            FilePane* rightPane = win->rightTabs()->currentFilePane();
            QVERIFY2(leftPane && rightPane, qPrintable(stage + QStringLiteral(": missing panes")));
            QVERIFY2(rightPane->endpoint().isRemote(), qPrintable(stage + QStringLiteral(": right pane not remote")));

            const int leftW = win->leftTabs()->width();
            const int rightW = win->rightTabs()->width();
            QVERIFY2(std::abs(leftW - rightW) <= 4,
                     qPrintable(QStringLiteral("[%1] Dual-pane splitter skewed: Left=%2 vs Right=%3 (diff=%4)")
                                    .arg(stage).arg(leftW).arg(rightW).arg(rightW - leftW)));

            auto* leftHeader = leftPane->findChild<QFrame*>(QStringLiteral("paneHeader"));
            auto* rightHeader = rightPane->findChild<QFrame*>(QStringLiteral("paneHeader"));
            auto* leftTable = leftPane->findChild<FileTableView*>();
            auto* rightTable = rightPane->findChild<FileTableView*>();
            auto* titleLbl = rightPane->findChild<QLabel*>(QStringLiteral("hostTitleLabel"));
            QVERIFY2(leftHeader && rightHeader && leftTable && rightTable && titleLbl,
                     qPrintable(stage + QStringLiteral(": missing header/table/title children")));

            QCOMPARE(leftHeader->height(), 36);
            QCOMPARE(rightHeader->height(), 36);
            const int leftTableY = leftTable->mapTo(win, QPoint(0, 0)).y();
            const int rightTableY = rightTable->mapTo(win, QPoint(0, 0)).y();
            QCOMPARE(leftTableY, rightTableY);

            QVERIFY2(titleLbl->toolTip().contains(expectedName),
                     qPrintable(QStringLiteral("[%1] hostTitleLabel tooltip missing '%2': '%3'")
                                    .arg(stage, expectedName, titleLbl->toolTip())));
            if (expectElided) {
                QVERIFY2(titleLbl->text().endsWith(QChar(0x2026)),
                         qPrintable(QStringLiteral("[%1] Long hostname did not elide with U+2026: '%2'")
                                        .arg(stage, titleLbl->text())));
            } else {
                QVERIFY2(!titleLbl->text().endsWith(QChar(0x2026)),
                         qPrintable(QStringLiteral("[%1] Short hostname unexpectedly elided: '%2'")
                                        .arg(stage, titleLbl->text())));
            }
        };

        for (bool dark : {true, false}) {
            ThemeManager::applyTheme(dark);
            const QString tag = dark ? QStringLiteral("dark") : QStringLiteral("light");

            // Case 1: New Window constructed with long FQDN initialRightSession -> show() at 1340x840,
            // then resize to 1024x768 after show().
            {
                std::unique_ptr<MainWindow> win(new MainWindow(&store, &transferMgr, {}, &longRemote));
                win->setAttribute(Qt::WA_DeleteOnClose, false);
                win->show();
                QCoreApplication::processEvents();
                QTest::qWait(40);
                verifySplitWindow(win.get(), longRemote.name, true, tag + QStringLiteral("_long_1340_initial"));
                saveShot(win->grab(), QStringLiteral("14_new_window_long_hostname_1340_%1.png").arg(tag));

                win->resize(1024, 768);
                QCoreApplication::processEvents();
                QTest::qWait(40);
                verifySplitWindow(win.get(), longRemote.name, true, tag + QStringLiteral("_long_1024_after_show"));

                win->close();
                QCoreApplication::processEvents();
            }

            // Case 2: New Window constructed with long FQDN initialRightSession + resize(1024, 768) BEFORE show().
            {
                std::unique_ptr<MainWindow> win(new MainWindow(&store, &transferMgr, {}, &longRemote));
                win->setAttribute(Qt::WA_DeleteOnClose, false);
                win->resize(1024, 768);
                win->show();
                QCoreApplication::processEvents();
                QTest::qWait(40);
                verifySplitWindow(win.get(), longRemote.name, true, tag + QStringLiteral("_long_1024_before_show"));

                win->close();
                QCoreApplication::processEvents();
            }

            // Case 3: Short remote initialRightSession stays 50/50 balanced without elision.
            {
                std::unique_ptr<MainWindow> win(new MainWindow(&store, &transferMgr, {}, &shortRemote));
                win->setAttribute(Qt::WA_DeleteOnClose, false);
                win->show();
                QCoreApplication::processEvents();
                QTest::qWait(40);
                verifySplitWindow(win.get(), shortRemote.name, false, tag + QStringLiteral("_short_1340_initial"));

                win->close();
                QCoreApplication::processEvents();
            }
        }

        transferMgr.shutdown();
    }
};

QTEST_MAIN(TestUi)
#include "test_ui.moc"
