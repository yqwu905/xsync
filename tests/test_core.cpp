#include "core/ProtocolNegotiator.h"
#include "core/SecretStore.h"
#include "core/SessionStore.h"
#include "core/ShellUtils.h"
#include "core/Types.h"
#include "ui/FileListModel.h"
#include "ui/FileTableView.h"
#include "ui/SessionEditDialog.h"

#include <QDir>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace xsync;

class TestCore : public QObject {
    Q_OBJECT
private slots:
    void testShellQuotePosix() {
        QCOMPARE(ShellUtils::shellQuotePosix(QStringLiteral("/tmp/hello world.txt")),
                 QStringLiteral("'/tmp/hello world.txt'"));
        QCOMPARE(ShellUtils::shellQuotePosix(QStringLiteral("a'b;rm -rf /")),
                 QStringLiteral("'a'\\''b;rm -rf /'"));
        QCOMPARE(ShellUtils::shellQuotePosix(QStringLiteral("$(whoami)`id`")),
                 QStringLiteral("'$(whoami)`id`'"));
    }

    void testWindowsRsyncPathConversion() {
        QCOMPARE(ShellUtils::toRsyncLocalPath(QStringLiteral("C:\\Users\\Alice\\My Data\\a.bin"),
                                              RsyncFlavor::Cygwin),
                 QStringLiteral("/cygdrive/c/Users/Alice/My Data/a.bin"));
        QCOMPARE(ShellUtils::toRsyncLocalPath(QStringLiteral("D:/Projects/xsync"),
                                              RsyncFlavor::Msys2),
                 QStringLiteral("/d/Projects/xsync"));
        QCOMPARE(ShellUtils::toRsyncLocalPath(QStringLiteral("E:\\Datasets\\img"),
                                              RsyncFlavor::Wsl),
                 QStringLiteral("/mnt/e/Datasets/img"));
        QCOMPARE(ShellUtils::toRsyncLocalPath(QStringLiteral("/home/user/file"),
                                              RsyncFlavor::NativePosix),
                 QStringLiteral("/home/user/file"));
    }

    void testParseRsyncProgress2() {
        const QString chunk = QStringLiteral(
            "   1,048,576  10%   12.50MB/s    0:00:08\r"
            "  52,428,800  50%   64.00MB/s    0:00:04 (xfr#1, to-chk=0/1)");
        auto parsed = ShellUtils::parseRsyncProgress2(chunk);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->bytesTransferred, uint64_t(52428800));
        QCOMPARE(parsed->percent, 50);
        QVERIFY(std::abs(parsed->speedBytesPerSec - 64.0 * 1024.0 * 1024.0) < 1024.0);
        QCOMPARE(parsed->etaText, QStringLiteral("0:00:04"));
    }

    void testSshConfigParser() {
        const QString cfg = QStringLiteral(R"(
# Global wildcard should be skipped
Host *
    ServerAliveInterval 60

Host gpu-train
    HostName 10.0.1.50
    Port 2222
    User mlops
    IdentityFile ~/.ssh/id_ed25519
    ProxyJump bastion@jump.corp.local:2200
    ForwardAgent yes
)");
        auto list = ShellUtils::parseSshConfig(cfg);
        QCOMPARE(list.size(), size_t(1));
        QCOMPARE(list[0].name, QStringLiteral("gpu-train"));
        QCOMPARE(list[0].host, QStringLiteral("10.0.1.50"));
        QCOMPARE(list[0].port, uint16_t(2222));
        QCOMPARE(list[0].username, QStringLiteral("mlops"));
        QCOMPARE(list[0].authType, AuthType::PrivateKey);
        QVERIFY(list[0].useJumpHost);
        QCOMPARE(list[0].jumpHost, QStringLiteral("jump.corp.local"));
        QCOMPARE(list[0].jumpPort, uint16_t(2200));
        QCOMPARE(list[0].jumpUsername, QStringLiteral("bastion"));
        QVERIFY(list[0].forwardAgent);
    }

    void testSecretStoreRoundTrip() {
        const QString secret = QStringLiteral("P@ssw0rd!中文#2026");
        const QString enc = SecretStore::protect(secret);
        QVERIFY(!enc.isEmpty());
        QVERIFY(!enc.contains(secret));
        QCOMPARE(SecretStore::unprotect(enc), secret);
        QCOMPARE(SecretStore::unprotect(QStringLiteral("corrupted-value")), QString());
    }

    void testSessionStorePersistence() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString storePath = dir.filePath(QStringLiteral("sessions.json"));

        SessionProfile p = SessionProfile::createNew();
        p.name = QStringLiteral("Prod-01");
        p.group = QStringLiteral("Production");
        p.host = QStringLiteral("10.20.30.40");
        p.port = 2202;
        p.username = QStringLiteral("deploy");
        p.authType = AuthType::Password;
        p.password = QStringLiteral("super-secret");
        p.protocolPreference = ProtocolPreference::Auto;

        {
            SessionStore s1(storePath);
            s1.upsertSession(p);
        }

        // Verify password is NOT stored in plaintext on disk
        QFile raw(storePath);
        QVERIFY(raw.open(QIODevice::ReadOnly));
        const QByteArray rawData = raw.readAll();
        QVERIFY(!rawData.contains("super-secret"));

        SessionStore s2(storePath);
        QVERIFY(s2.load());
        QCOMPARE(s2.sessions().size(), size_t(1));
        QCOMPARE(s2.sessions()[0].password, QStringLiteral("super-secret"));
        QCOMPARE(s2.sessions()[0].port, uint16_t(2202));
    }

    void testProtocolNegotiationMatrix() {
        SessionProfile rA = SessionProfile::createNew();
        rA.host = QStringLiteral("a.example.com");
        rA.username = QStringLiteral("u");
        SessionProfile rB = SessionProfile::createNew();
        rB.host = QStringLiteral("b.example.com");
        rB.username = QStringLiteral("u");

        EndpointSpec local = EndpointSpec::local();
        EndpointSpec epA = EndpointSpec::remote(rA);
        EndpointSpec epB = EndpointSpec::remote(rB);

        EndpointCapabilities fullCaps;
        fullCaps.probed = true;
        fullCaps.hasRsync = true;
        fullCaps.hasSftp = true;
        fullCaps.hasScp = true;
        fullCaps.hasSsh = true;

        EndpointCapabilities noRsyncCaps = fullCaps;
        noRsyncCaps.hasRsync = false;

        EndpointCapabilities scpOnlyCaps;
        scpOnlyCaps.probed = true;
        scpOnlyCaps.hasScp = true;

        // 1. Local <-> Remote: both have rsync => rsync primary, SFTP & SCP fallbacks
        {
            auto plan = ProtocolNegotiator::negotiate(
                local, epA, fullCaps, fullCaps,
                ProtocolPreference::Auto, RemoteToRemotePolicy::DirectThenRelay);
            QCOMPARE(plan.primary, TransferProtocol::Rsync);
            QVERIFY(plan.fallbacks.size() == 2);
            QCOMPARE(plan.fallbacks[0], TransferProtocol::Sftp);
            QCOMPARE(plan.fallbacks[1], TransferProtocol::Scp);
        }

        // 2. Local <-> Remote: local has no rsync => degrades to SFTP
        {
            auto plan = ProtocolNegotiator::negotiate(
                local, epA, noRsyncCaps, fullCaps,
                ProtocolPreference::Auto, RemoteToRemotePolicy::DirectThenRelay);
            QCOMPARE(plan.primary, TransferProtocol::Sftp);
            QCOMPARE(plan.fallbacks[0], TransferProtocol::Scp);
        }

        // 3. Local <-> Remote: remote has neither rsync nor SFTP => degrades to SCP
        {
            auto plan = ProtocolNegotiator::negotiate(
                local, epA, fullCaps, scpOnlyCaps,
                ProtocolPreference::Auto, RemoteToRemotePolicy::DirectThenRelay);
            QCOMPARE(plan.primary, TransferProtocol::Scp);
        }

        // 4. Remote <-> Remote (DirectThenRelay): both have rsync => Direct rsync -> Direct SCP -> Relay SFTP -> Relay SCP
        {
            auto plan = ProtocolNegotiator::negotiate(
                epA, epB, fullCaps, fullCaps,
                ProtocolPreference::Auto, RemoteToRemotePolicy::DirectThenRelay);
            QCOMPARE(plan.primary, TransferProtocol::RemoteDirectRsync);
            QVERIFY(plan.fallbacks.size() >= 2);
            QCOMPARE(plan.fallbacks[0], TransferProtocol::RemoteDirectScp);
            QCOMPARE(plan.fallbacks[1], TransferProtocol::RemoteRelaySftp);
        }

        // 5. Remote <-> Remote (DirectThenRelay): one remote lacks rsync => Direct SCP -> Relay SFTP
        {
            auto plan = ProtocolNegotiator::negotiate(
                epA, epB, fullCaps, noRsyncCaps,
                ProtocolPreference::Auto, RemoteToRemotePolicy::DirectThenRelay);
            QCOMPARE(plan.primary, TransferProtocol::RemoteDirectScp);
            QCOMPARE(plan.fallbacks[0], TransferProtocol::RemoteRelaySftp);
        }

        // 6. Remote <-> Remote (RelayOnly): skips direct completely -> Relay SFTP
        {
            auto plan = ProtocolNegotiator::negotiate(
                epA, epB, fullCaps, fullCaps,
                ProtocolPreference::Auto, RemoteToRemotePolicy::RelayOnly);
            QCOMPARE(plan.primary, TransferProtocol::RemoteRelaySftp);
        }

        // 7. Remote <-> Remote (DirectOnly): never includes relay fallbacks
        {
            auto plan = ProtocolNegotiator::negotiate(
                epA, epB, fullCaps, fullCaps,
                ProtocolPreference::Auto, RemoteToRemotePolicy::DirectOnly);
            QCOMPARE(plan.primary, TransferProtocol::RemoteDirectRsync);
            for (auto fb : plan.fallbacks) {
                QVERIFY(fb != TransferProtocol::RemoteRelaySftp);
                QVERIFY(fb != TransferProtocol::RemoteRelayScp);
            }
        }
    }

    void testFileListModelSortingAndFiltering() {
        FileListModel model;
        FileSortFilterProxyModel proxy;
        proxy.setSourceModel(&model);

        std::vector<FileEntry> entries;
        entries.push_back({QStringLiteral("zeta.txt"), QStringLiteral("/zeta.txt"), 100, 1000, 0644, false, false, false, {}, {}, {}});
        entries.push_back({QStringLiteral("alpha_dir"), QStringLiteral("/alpha_dir"), 0, 1000, 0755, true, false, false, {}, {}, {}});
        entries.push_back({QStringLiteral(".hidden"), QStringLiteral("/.hidden"), 50, 1000, 0644, false, false, false, {}, {}, {}});
        entries.push_back({QStringLiteral("beta.log"), QStringLiteral("/beta.log"), 500, 1000, 0644, false, false, false, {}, {}, {}});

        model.setEntries(entries, true, QStringLiteral("/"));
        proxy.sort(FileListModel::ColName, Qt::AscendingOrder);

        QCOMPARE(proxy.rowCount(), 5);
        // ".." always pinned first, then directories, then files
        QCOMPARE(proxy.data(proxy.index(0, 0)).toString(), QStringLiteral(".."));
        QCOMPARE(proxy.data(proxy.index(1, 0)).toString(), QStringLiteral("alpha_dir"));

        // Descending sort MUST still keep ".." at row 0 and dirs above files
        proxy.sort(FileListModel::ColName, Qt::DescendingOrder);
        QCOMPARE(proxy.data(proxy.index(0, 0)).toString(), QStringLiteral(".."));
        QCOMPARE(proxy.data(proxy.index(1, 0)).toString(), QStringLiteral("alpha_dir"));

        // Hide dotfiles
        proxy.setShowHidden(false);
        QCOMPARE(proxy.rowCount(), 4);

        // Name filter keeps ".." + matching entry
        proxy.setNameFilter(QStringLiteral("beta"));
        QCOMPARE(proxy.rowCount(), 2);
        QCOMPARE(proxy.data(proxy.index(1, 0)).toString(), QStringLiteral("beta.log"));
    }

    void testMimePayloadRoundTrip() {
        SessionProfile sp = SessionProfile::createNew();
        sp.name = QStringLiteral("NodeA");
        sp.host = QStringLiteral("10.0.0.5");
        sp.port = 2222;
        sp.username = QStringLiteral("ubuntu");
        sp.password = QStringLiteral("pass123");

        XsyncMimePayload orig;
        orig.sourceEndpoint = EndpointSpec::remote(sp);
        orig.sourceDir = QStringLiteral("/data/models");
        orig.isCut = true;
        orig.entries.push_back({QStringLiteral("weights.pt"), QStringLiteral("/data/models/weights.pt"),
                                1048576, 0, 0644, false, false, false, {}, {}, {}});

        const QByteArray encoded = FileTableView::encodePayload(orig);
        QVERIFY(!encoded.contains("pass123"));  // Password encrypted inside MIME too!

        auto decoded = FileTableView::decodePayload(encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->sourceEndpoint.session.host, QStringLiteral("10.0.0.5"));
        QCOMPARE(decoded->sourceEndpoint.session.password, QStringLiteral("pass123"));
        QCOMPARE(decoded->isCut, true);
        QCOMPARE(decoded->entries.size(), size_t(1));
        QCOMPARE(decoded->entries[0].fullPath, QStringLiteral("/data/models/weights.pt"));
    }

    void testSessionEditBrowseAlwaysEnabled() {
        // Default new profile has AuthType::Password — Browse... button MUST still be enabled
        SessionProfile p = SessionProfile::createNew();
        QCOMPARE(p.authType, AuthType::Password);

        SessionEditDialog dlg(p, {QStringLiteral("Default")});
        auto* browseBtn = dlg.findChild<QPushButton*>(QStringLiteral("keyBrowseBtn"));
        QVERIFY(browseBtn != nullptr);
        QVERIFY(browseBtn->isEnabled());
    }

    void testPerHostPathBookmarksPersistence() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString storePath = dir.filePath(QStringLiteral("sessions.json"));

        SessionProfile hostA = SessionProfile::createNew();
        hostA.name = QStringLiteral("GPU-A");
        hostA.host = QStringLiteral("10.0.0.10");
        hostA.username = QStringLiteral("ubuntu");

        SessionProfile hostB = SessionProfile::createNew();
        hostB.name = QStringLiteral("GPU-B");
        hostB.host = QStringLiteral("10.0.0.20");
        hostB.username = QStringLiteral("ubuntu");

        SessionProfile quickC = SessionProfile::createNew();
        quickC.host = QStringLiteral("10.0.0.30");
        quickC.username = QStringLiteral("root");

        EndpointSpec epA = EndpointSpec::remote(hostA);
        EndpointSpec epB = EndpointSpec::remote(hostB);
        EndpointSpec epQuickC = EndpointSpec::remote(quickC);
        EndpointSpec epLocal = EndpointSpec::local();

        {
            SessionStore s1(storePath);
            s1.upsertSession(hostA);
            s1.upsertSession(hostB);

            s1.addBookmark(epA, {QStringLiteral("Checkpoints"), QStringLiteral("/data/ckpts")});
            s1.addBookmark(epA, {QStringLiteral("Logs"), QStringLiteral("/var/log/train")});
            s1.addBookmark(epB, {QStringLiteral("Datasets"), QStringLiteral("/mnt/datasets")});
            s1.addBookmark(epQuickC, {QStringLiteral("Tmp"), QStringLiteral("/tmp")});
            s1.addBookmark(epLocal, {QStringLiteral("Home"), QDir::homePath()});

            QVERIFY(s1.hasBookmark(epA, QStringLiteral("/data/ckpts")));
            QVERIFY(!s1.hasBookmark(epB, QStringLiteral("/data/ckpts")));
            QCOMPARE(s1.bookmarksFor(epA).size(), size_t(2));
            QCOMPARE(s1.bookmarksFor(epB).size(), size_t(1));
        }

        // Reload from disk and verify per-host persistence & isolation
        SessionStore s2(storePath);
        QVERIFY(s2.load());
        QCOMPARE(s2.bookmarksFor(epA).size(), size_t(2));
        QCOMPARE(s2.bookmarksFor(epA)[0].name, QStringLiteral("Checkpoints"));
        QCOMPARE(s2.bookmarksFor(epA)[0].path, QStringLiteral("/data/ckpts"));
        QCOMPARE(s2.bookmarksFor(epB).size(), size_t(1));
        QCOMPARE(s2.bookmarksFor(epB)[0].path, QStringLiteral("/mnt/datasets"));
        QCOMPARE(s2.bookmarksFor(epQuickC).size(), size_t(1));
        QCOMPARE(s2.bookmarksFor(epLocal).size(), size_t(1));

        // Remove one bookmark on Host A without affecting Host B
        QVERIFY(s2.removeBookmark(epA, QStringLiteral("/data/ckpts")));
        QCOMPARE(s2.bookmarksFor(epA).size(), size_t(1));
        QCOMPARE(s2.bookmarksFor(epB).size(), size_t(1));
    }
};

QTEST_MAIN(TestCore)
#include "test_core.moc"
