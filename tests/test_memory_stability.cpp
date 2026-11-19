#include "core/MemoryStats.h"
#include "core/TransferManager.h"
#include "core/Types.h"
#include "ui/FileListModel.h"

#include <QPlainTextEdit>
#include <QtTest/QtTest>

using namespace xsync;

// Verifies xsync's structural anti-memory-leak invariants under sustained load.
class TestMemoryStability : public QObject {
    Q_OBJECT
private slots:
    void testFileListModelShrinkAndFlatRss() {
        FileListModel model;

        // Warm up once
        {
            std::vector<FileEntry> warm(5000);
            for (size_t i = 0; i < warm.size(); ++i) {
                warm[i].name = QStringLiteral("file_%1.bin").arg(i);
                warm[i].fullPath = QStringLiteral("/data/file_%1.bin").arg(i);
                warm[i].size = i * 1024;
            }
            model.setEntries(std::move(warm), true, QStringLiteral("/"));
        }
        const double baselineMb = MemoryStats::currentRssMegabytes();

        // Simulate browsing 300 large directories (1.5M FileEntry objects total)
        for (int iter = 0; iter < 300; ++iter) {
            std::vector<FileEntry> batch(5000);
            for (size_t i = 0; i < batch.size(); ++i) {
                batch[i].name = QStringLiteral("iter_%1_item_%2.dat").arg(iter).arg(i);
                batch[i].fullPath = QStringLiteral("/mnt/d%1/item_%2.dat").arg(iter).arg(i);
                batch[i].size = 4096;
            }
            model.setEntries(std::move(batch), true, QStringLiteral("/"));
        }

        // Navigate into a small folder -> capacity MUST shrink back down
        std::vector<FileEntry> tiny(5);
        for (size_t i = 0; i < tiny.size(); ++i) {
            tiny[i].name = QStringLiteral("small_%1.txt").arg(i);
            tiny[i].fullPath = QStringLiteral("/small_%1.txt").arg(i);
        }
        model.setEntries(std::move(tiny), true, QStringLiteral("/"));
        QVERIFY2(model.vectorCapacity() < 128,
                 qPrintable(QStringLiteral("Expected shrunk vector capacity < 128, got %1")
                                .arg(model.vectorCapacity())));

        MemoryStats::trimProcessMemory();
        const double afterMb = MemoryStats::currentRssMegabytes();
        const double deltaMb = afterMb - baselineMb;
        qInfo("Memory stability check: baseline=%.2f MB, after 1.5M entries=%.2f MB, delta=%.2f MB",
              baselineMb, afterMb, deltaMb);
        QVERIFY2(deltaMb < 15.0,
                 qPrintable(QStringLiteral("RSS grew unexpectedly by %1 MB").arg(deltaMb)));
    }

    void testTransferHistoryBoundedEviction() {
        TransferManager mgr;
        mgr.setPaused(true);
        mgr.setMaxHistorySize(50);

        // Enqueue 400 tasks and cancel them -> finished history must strictly cap at 50
        for (int i = 0; i < 400; ++i) {
            TransferTask t;
            t.sourceEndpoint = EndpointSpec::local();
            t.destEndpoint = EndpointSpec::local();
            t.sourcePath = QStringLiteral("/tmp/f_%1").arg(i);
            t.destDir = QStringLiteral("/tmp/dst");
            t.itemName = QStringLiteral("f_%1").arg(i);
            uint64_t id = mgr.enqueueTask(std::move(t));
            mgr.cancelTask(id);
        }
        QVERIFY2(mgr.tasks().size() <= 50,
                 qPrintable(QStringLiteral("Expected <= 50 bounded history tasks, got %1")
                                .arg(mgr.tasks().size())));
    }

    void testBoundedActivityLogWidget() {
        QPlainTextEdit logView;
        logView.setMaximumBlockCount(kMaxLogLines);
        for (int i = 0; i < 5000; ++i) {
            logView.appendPlainText(QStringLiteral("Transfer log line #%1").arg(i));
        }
        QVERIFY(logView.blockCount() <= kMaxLogLines);
    }
};

QTEST_MAIN(TestMemoryStability)
#include "test_memory_stability.moc"
