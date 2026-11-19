#include "core/FileSystemBackend.h"
#include "core/ProtocolNegotiator.h"
#include "core/TransferManager.h"
#include "ui/FileListModel.h"
#include "ui/FileTableView.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest/QtTest>

using namespace xsync;

class TestTransferLocal : public QObject {
    Q_OBJECT
private slots:
    void testLocalFileAndDirectoryTransfer() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        const QString srcRoot = tmp.filePath(QStringLiteral("src"));
        const QString dstRoot = tmp.filePath(QStringLiteral("dst"));
        QDir().mkpath(srcRoot + QStringLiteral("/sub/nested"));
        QDir().mkpath(dstRoot);

        // Write a 2 MB binary file (larger than 256 KiB buffer to exercise multi-chunk loop)
        QByteArray payload(2 * 1024 * 1024 + 137, '\0');
        for (int i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>((i * 31) & 0xFF);
        }
        {
            QFile f(srcRoot + QStringLiteral("/big.bin"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            QCOMPARE(f.write(payload), qint64(payload.size()));
        }
        {
            QFile f(srcRoot + QStringLiteral("/sub/nested/hello.txt"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("hello xsync");
        }

        TransferManager mgr;
        int completedCount = 0;
        connect(&mgr, &TransferManager::taskUpdated, this, [&](const TransferTask& t) {
            if (t.status == TransferStatus::Completed) ++completedCount;
        });

        // 1. Copy 2 MB file
        TransferTask t1;
        t1.sourceEndpoint = EndpointSpec::local();
        t1.destEndpoint = EndpointSpec::local();
        t1.sourcePath = srcRoot + QStringLiteral("/big.bin");
        t1.destDir = dstRoot;
        t1.itemName = QStringLiteral("big.bin");
        mgr.enqueueTask(t1);

        // 2. Copy nested directory recursively
        TransferTask t2;
        t2.sourceEndpoint = EndpointSpec::local();
        t2.destEndpoint = EndpointSpec::local();
        t2.sourcePath = srcRoot + QStringLiteral("/sub");
        t2.destDir = dstRoot;
        t2.itemName = QStringLiteral("sub");
        t2.isDir = true;
        mgr.enqueueTask(t2);

        QTRY_COMPARE_WITH_TIMEOUT(completedCount, 2, 10000);

        // Verify byte-exact copy
        QFile dstBig(dstRoot + QStringLiteral("/big.bin"));
        QVERIFY(dstBig.open(QIODevice::ReadOnly));
        QCOMPARE(dstBig.readAll(), payload);

        QFile dstNested(dstRoot + QStringLiteral("/sub/nested/hello.txt"));
        QVERIFY(dstNested.open(QIODevice::ReadOnly));
        QCOMPARE(dstNested.readAll(), QByteArray("hello xsync"));

        // 3. Test Cut (deleteSourceAfter)
        TransferTask t3;
        t3.sourceEndpoint = EndpointSpec::local();
        t3.destEndpoint = EndpointSpec::local();
        t3.sourcePath = srcRoot + QStringLiteral("/big.bin");
        t3.destDir = dstRoot + QStringLiteral("/moved");
        t3.itemName = QStringLiteral("big.bin");
        t3.deleteSourceAfter = true;
        QDir().mkpath(t3.destDir);
        mgr.enqueueTask(t3);

        QTRY_COMPARE_WITH_TIMEOUT(completedCount, 3, 10000);
        QVERIFY(!QFile::exists(srcRoot + QStringLiteral("/big.bin")));
        QVERIFY(QFile::exists(dstRoot + QStringLiteral("/moved/big.bin")));
    }

    void testExplorerClipboardCtrlCVIntegration() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString f1 = tmp.filePath(QStringLiteral("doc1.txt"));
        {
            QFile f(f1);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("clipboard test");
        }

        // Simulate Windows Explorer Ctrl+C placing file URLs onto OS clipboard
        auto* explorerMime = new QMimeData();
        explorerMime->setUrls({QUrl::fromLocalFile(f1)});
        QApplication::clipboard()->setMimeData(explorerMime);

        // Target Remote FileTableView receives Ctrl+V paste
        SessionProfile remoteProf = SessionProfile::createNew();
        remoteProf.host = QStringLiteral("10.0.0.1");
        remoteProf.username = QStringLiteral("alice");

        FileTableView view;
        view.setContextInfo(EndpointSpec::remote(remoteProf), QStringLiteral("/home/alice/uploads"));

        QSignalSpy spy(&view, &FileTableView::externalLocalFilesDropped);
        view.pasteFromClipboard();

        QCOMPARE(spy.count(), 1);
        const QStringList droppedPaths = spy.at(0).at(0).toStringList();
        const QString targetDir = spy.at(0).at(1).toString();
        QCOMPARE(droppedPaths.size(), 1);
        QCOMPARE(QDir::cleanPath(droppedPaths[0]), QDir::cleanPath(f1));
        QCOMPARE(targetDir, QStringLiteral("/home/alice/uploads"));
    }

    void testLocalProbeFindsRsyncOnLinux() {
        auto caps = ProtocolNegotiator::probeLocal(true);
        QVERIFY(caps.probed);
        QVERIFY(caps.hasSftp);
        QVERIFY(caps.hasScp);
#ifndef _WIN32
        // In our Linux CI/VM environment, rsync and ssh are installed
        QVERIFY(caps.hasRsync);
        QVERIFY(caps.hasSsh);
#endif
    }
};

QTEST_MAIN(TestTransferLocal)
#include "test_transfer_local.moc"
