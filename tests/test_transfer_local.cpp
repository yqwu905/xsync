#include "core/FileSystemBackend.h"
#include "core/ProtocolNegotiator.h"
#include "core/TransferManager.h"
#include "ui/FileListModel.h"
#include "ui/FileTableView.h"
#include "ui/TransferQueueWidget.h"

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

        // 2. Copy nested directory recursively (expands into batch + 1 child file task)
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

    void testDirectoryBatchExpansionPauseRemoveAndErrors() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        const QString srcFolder = tmp.filePath(QStringLiteral("dataset"));
        const QString dstRoot1 = tmp.filePath(QStringLiteral("out1"));
        const QString dstRoot2 = tmp.filePath(QStringLiteral("out2"));
        QDir().mkpath(srcFolder + QStringLiteral("/nested"));
        QDir().mkpath(dstRoot1);
        QDir().mkpath(dstRoot2);

        const QStringList relFiles = {
            QStringLiteral("a.txt"),
            QStringLiteral("b.txt"),
            QStringLiteral("nested/c.txt"),
            QStringLiteral("nested/d.txt")
        };
        for (const QString& rel : relFiles) {
            QFile f(srcFolder + QLatin1Char('/') + rel);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(rel.toUtf8());
        }

        TransferManager mgr;
        TransferQueueModel model(&mgr);
        mgr.setPaused(true);

        // Enqueue directory while paused -> expands into 1 batch with 4 individual file children
        TransferTask dirTask;
        dirTask.sourceEndpoint = EndpointSpec::local();
        dirTask.destEndpoint = EndpointSpec::local();
        dirTask.sourcePath = srcFolder;
        dirTask.destDir = dstRoot1;
        dirTask.itemName = QStringLiteral("dataset");
        dirTask.isDir = true;
        const uint64_t batchId = mgr.enqueueTask(dirTask);

        QTRY_VERIFY_WITH_TIMEOUT(mgr.tasksInBatch(batchId).size() == 4, 5000);
        QCOMPARE(model.rowCount(), 1);
        const QModelIndex batchIdx = model.index(0, 0);
        QCOMPARE(model.rowCount(batchIdx), 4);
        QCOMPARE(model.data(model.index(0, TransferQueueModel::ColFiles)).toString(),
                 QStringLiteral("0 / 4"));

        // 1. Selective Pause & Selective Delete inside the directory batch!
        const auto children = mgr.tasksInBatch(batchId);
        const uint64_t pausedChildId = children[0].id;
        const QString pausedRel = children[0].relativePath;
        const uint64_t removedChildId = children[1].id;
        const QString removedRel = children[1].relativePath;

        mgr.pauseTask(pausedChildId);
        mgr.removeTask(removedChildId);

        QCOMPARE(model.rowCount(batchIdx), 3);
        const TransferBatch* bMid = mgr.findBatch(batchId);
        QVERIFY(bMid != nullptr);
        QCOMPARE(bMid->totalFiles, 3);
        QCOMPARE(bMid->pausedFiles, 1);

        // Resume queue -> remaining 2 non-paused files transfer, paused file stays Paused, deleted file skipped!
        mgr.setPaused(false);
        QTRY_VERIFY_WITH_TIMEOUT(mgr.findBatch(batchId) && mgr.findBatch(batchId)->completedFiles == 2, 5000);
        QCOMPARE(mgr.findBatch(batchId)->pausedFiles, 1);
        QCOMPARE(mgr.findBatch(batchId)->status, TransferStatus::Paused);
        QVERIFY(!QFile::exists(dstRoot1 + QStringLiteral("/dataset/") + removedRel));
        QVERIFY(!QFile::exists(dstRoot1 + QStringLiteral("/dataset/") + pausedRel));

        // Now resume the individually paused file -> batch completes 3 / 3!
        mgr.resumeTask(pausedChildId);
        QTRY_VERIFY_WITH_TIMEOUT(mgr.findBatch(batchId) && mgr.findBatch(batchId)->status == TransferStatus::Completed, 5000);
        QCOMPARE(model.data(model.index(0, TransferQueueModel::ColFiles)).toString(),
                 QStringLiteral("3 / 3"));
        QVERIFY(QFile::exists(dstRoot1 + QStringLiteral("/dataset/") + pausedRel));

        // 2. Per-file error reporting: missing source file records exact error while sibling succeeds
        TransferTask badFile;
        badFile.sourceEndpoint = EndpointSpec::local();
        badFile.destEndpoint = EndpointSpec::local();
        badFile.sourcePath = tmp.filePath(QStringLiteral("does_not_exist.bin"));
        badFile.destDir = dstRoot2;
        badFile.itemName = QStringLiteral("does_not_exist.bin");
        const uint64_t badId = mgr.enqueueTask(badFile);

        TransferTask goodFile;
        goodFile.sourceEndpoint = EndpointSpec::local();
        goodFile.destEndpoint = EndpointSpec::local();
        goodFile.sourcePath = srcFolder + QStringLiteral("/a.txt");
        goodFile.destDir = dstRoot2;
        goodFile.itemName = QStringLiteral("a.txt");
        const uint64_t goodId = mgr.enqueueTask(goodFile);

        QTRY_VERIFY_WITH_TIMEOUT(
            mgr.findTask(badId) && mgr.findTask(badId)->status == TransferStatus::Failed &&
            mgr.findTask(goodId) && mgr.findTask(goodId)->status == TransferStatus::Completed,
            5000);

        QVERIFY(!mgr.findTask(badId)->errorMessage.isEmpty());
        QCOMPARE(mgr.failedCount(), 1);

        // Verify TransferQueueFilterProxy FilterFailed isolates the failed file
        TransferQueueFilterProxy failedFilter;
        failedFilter.setSourceModel(&model);
        failedFilter.setFilterMode(TransferQueueFilterProxy::FilterFailed);
        QCOMPARE(failedFilter.rowCount(), 1);
        QVERIFY(failedFilter.data(failedFilter.index(0, TransferQueueModel::ColStatus))
                    .toString()
                    .contains(QStringLiteral("Error:")));
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
