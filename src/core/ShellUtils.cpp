#include "core/ShellUtils.h"

#include <QDir>
#include <QRegularExpression>
#include <QStringList>

namespace xsync {

QString ShellUtils::shellQuotePosix(const QString& arg) {
    QString out;
    out.reserve(arg.size() + 4);
    out.append(QLatin1Char('\''));
    for (QChar ch : arg) {
        if (ch == QLatin1Char('\'')) {
            out.append(QStringLiteral("'\\''"));
        } else {
            out.append(ch);
        }
    }
    out.append(QLatin1Char('\''));
    return out;
}

QString ShellUtils::toRsyncLocalPath(const QString& localPath, RsyncFlavor flavor) {
    if (flavor == RsyncFlavor::NativePosix || flavor == RsyncFlavor::None) {
        return localPath;
    }

    QString p = localPath;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // Match Windows drive letter: C:/... or C:
    if (p.size() >= 2 && p[1] == QLatin1Char(':') && p[0].isLetter()) {
        const QChar drive = p[0].toLower();
        QString rest = p.mid(2);
        if (!rest.startsWith(QLatin1Char('/'))) {
            rest.prepend(QLatin1Char('/'));
        }
        switch (flavor) {
            case RsyncFlavor::Cygwin:
                return QStringLiteral("/cygdrive/%1%2").arg(drive, rest);
            case RsyncFlavor::Msys2:
                return QStringLiteral("/%1%2").arg(drive, rest);
            case RsyncFlavor::Wsl:
                return QStringLiteral("/mnt/%1%2").arg(drive, rest);
            default:
                return p;
        }
    }
    return p;
}

std::optional<RsyncProgress> ShellUtils::parseRsyncProgress2(const QString& text) {
    // Handles rsync --info=progress2 lines such as:
    //   12,345,678  42%   54.32MB/s    0:00:12 (xfr#3, to-chk=10/14)
    //   12345678 100%  123.45kB/s    0:00:00
    static const QRegularExpression re(
        QStringLiteral(R"(([0-9][0-9,]*)\s+(\d{1,3})%\s+([0-9]+(?:\.[0-9]+)?)([kKMGT]?B)/s\s+(\d+:\d+:\d+))"));

    // Split by \r or \n and take the latest match in the chunk
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
    std::optional<RsyncProgress> latest;
    for (const QString& line : lines) {
        const QRegularExpressionMatch m = re.match(line);
        if (!m.hasMatch()) continue;

        RsyncProgress p;
        QString bytesStr = m.captured(1);
        bytesStr.remove(QLatin1Char(','));
        p.bytesTransferred = bytesStr.toULongLong();
        p.percent = qBound(0, m.captured(2).toInt(), 100);

        const double speedVal = m.captured(3).toDouble();
        const QString unit = m.captured(4).toLower();
        double mult = 1.0;
        if (unit == QLatin1String("kb")) mult = 1024.0;
        else if (unit == QLatin1String("mb")) mult = 1024.0 * 1024.0;
        else if (unit == QLatin1String("gb")) mult = 1024.0 * 1024.0 * 1024.0;
        else if (unit == QLatin1String("tb")) mult = 1024.0 * 1024.0 * 1024.0 * 1024.0;
        p.speedBytesPerSec = speedVal * mult;
        p.etaText = m.captured(5);
        latest = p;
    }
    return latest;
}

QString ShellUtils::expandTilde(const QString& path) {
    if (path == QLatin1String("~")) {
        return QDir::homePath();
    }
    if (path.startsWith(QLatin1String("~/")) || path.startsWith(QLatin1String("~\\"))) {
        return QDir::homePath() + QLatin1Char('/') + path.mid(2);
    }
    return path;
}

std::vector<SessionProfile> ShellUtils::parseSshConfig(const QString& configText) {
    struct RawBlock {
        QString alias;
        QString hostName;
        uint16_t port = 22;
        QString user;
        QString identityFile;
        QString proxyJump;
        bool forwardAgent = false;
    };

    std::vector<RawBlock> blocks;
    std::vector<size_t> activeIndices;

    const QStringList lines = configText.split(QLatin1Char('\n'));
    for (QString rawLine : lines) {
        // Strip comments
        int hashIdx = rawLine.indexOf(QLatin1Char('#'));
        if (hashIdx >= 0) rawLine = rawLine.left(hashIdx);
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) continue;

        // Key can be separated by whitespace or '='
        static const QRegularExpression kvRe(QStringLiteral(R"(^([A-Za-z0-9_]+)(?:\s*=\s*|\s+)(.+)$)"));
        const QRegularExpressionMatch m = kvRe.match(line);
        if (!m.hasMatch()) continue;

        const QString key = m.captured(1).toLower();
        QString val = m.captured(2).trimmed();
        if (val.size() >= 2 && val.startsWith(QLatin1Char('"')) && val.endsWith(QLatin1Char('"'))) {
            val = val.mid(1, val.size() - 2);
        }

        if (key == QLatin1String("host")) {
            activeIndices.clear();
            const QStringList aliases = val.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            for (const QString& alias : aliases) {
                if (alias.contains(QLatin1Char('*')) || alias.contains(QLatin1Char('?')) ||
                    alias.startsWith(QLatin1Char('!'))) {
                    continue;
                }
                RawBlock b;
                b.alias = alias;
                blocks.push_back(b);
                activeIndices.push_back(blocks.size() - 1);
            }
        } else if (!activeIndices.empty()) {
            for (size_t idx : activeIndices) {
                RawBlock& b = blocks[idx];
                if (key == QLatin1String("hostname") && b.hostName.isEmpty()) {
                    b.hostName = val;
                } else if (key == QLatin1String("port")) {
                    bool ok = false;
                    uint p = val.toUInt(&ok);
                    if (ok && p > 0 && p <= 65535) b.port = static_cast<uint16_t>(p);
                } else if (key == QLatin1String("user") && b.user.isEmpty()) {
                    b.user = val;
                } else if (key == QLatin1String("identityfile") && b.identityFile.isEmpty()) {
                    b.identityFile = expandTilde(val);
                } else if (key == QLatin1String("proxyjump") && b.proxyJump.isEmpty()) {
                    b.proxyJump = val;
                } else if (key == QLatin1String("forwardagent")) {
                    b.forwardAgent = (val.toLower() == QLatin1String("yes"));
                }
            }
        }
    }

    std::vector<SessionProfile> result;
    result.reserve(blocks.size());
    const QString fallbackUser = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME", QStringLiteral("root")));

    for (const RawBlock& b : blocks) {
        SessionProfile p = SessionProfile::createNew();
        p.name = b.alias;
        p.group = QStringLiteral("SSH Config");
        p.host = b.hostName.isEmpty() ? b.alias : b.hostName;
        p.port = b.port;
        p.username = b.user.isEmpty() ? fallbackUser : b.user;
        p.forwardAgent = b.forwardAgent;

        if (!b.identityFile.isEmpty()) {
            p.authType = AuthType::PrivateKey;
            p.privateKeyPath = b.identityFile;
        } else {
            p.authType = AuthType::SshAgent;
        }

        if (!b.proxyJump.isEmpty() && b.proxyJump.toLower() != QLatin1String("none")) {
            // Parse first jump hop: [user@]host[:port]
            QString hop = b.proxyJump.section(QLatin1Char(','), 0, 0).trimmed();
            p.useJumpHost = true;
            if (hop.contains(QLatin1Char('@'))) {
                p.jumpUsername = hop.section(QLatin1Char('@'), 0, 0);
                hop = hop.section(QLatin1Char('@'), 1);
            } else {
                p.jumpUsername = p.username;
            }
            if (hop.contains(QLatin1Char(':'))) {
                p.jumpHost = hop.section(QLatin1Char(':'), 0, 0);
                bool ok = false;
                uint jp = hop.section(QLatin1Char(':'), 1).toUInt(&ok);
                p.jumpPort = (ok && jp > 0 && jp <= 65535) ? static_cast<uint16_t>(jp) : 22;
            } else {
                p.jumpHost = hop;
                p.jumpPort = 22;
            }
            p.jumpAuthType = p.authType;
            p.jumpPrivateKeyPath = p.privateKeyPath;
        }

        result.push_back(std::move(p));
    }
    return result;
}

}  // namespace xsync
