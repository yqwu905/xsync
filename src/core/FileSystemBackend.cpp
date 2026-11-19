#include "core/FileSystemBackend.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace xsync {

QString LocalFileSystem::defaultHomePath() {
    QString home = QDir::homePath();
    return home.isEmpty() ? QDir::rootPath() : QDir::cleanPath(home);
}

QString LocalFileSystem::normalizePath(const QString& path) {
    QString p = path.trimmed();
    if (p.isEmpty()) return defaultHomePath();
#ifdef _WIN32
    if (p == QLatin1String("/") || p == QLatin1String("\\") || p == QLatin1String("Computer")) {
        return QStringLiteral("/");  // Virtual drives list root on Windows
    }
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (p.size() == 2 && p[1] == QLatin1Char(':')) {
        p.append(QLatin1Char('/'));
    }
    return QDir::cleanPath(p);
#else
    return QDir::cleanPath(p);
#endif
}

QString LocalFileSystem::parentPath(const QString& path) {
    QString norm = normalizePath(path);
#ifdef _WIN32
    if (norm == QLatin1String("/")) return norm;
    // Going up from a drive root like "C:/" goes to virtual drives list "/"
    if (norm.size() <= 3 && norm.size() >= 2 && norm[1] == QLatin1Char(':')) {
        return QStringLiteral("/");
    }
#endif
    QDir d(norm);
    if (!d.cdUp()) {
#ifdef _WIN32
        return QStringLiteral("/");
#else
        return QStringLiteral("/");
#endif
    }
    return QDir::cleanPath(d.absolutePath());
}

QString LocalFileSystem::joinPath(const QString& dir, const QString& name) {
    QString base = normalizePath(dir);
    if (base.endsWith(QLatin1Char('/'))) {
        return base + name;
    }
    return base + QLatin1Char('/') + name;
}

bool LocalFileSystem::listDirectory(const QString& dirPath,
                                    std::vector<FileEntry>& outEntries,
                                    QString& errorOut) {
    outEntries.clear();
    const QString norm = normalizePath(dirPath);

#ifdef _WIN32
    // Virtual drive root listing on Windows (like Xftp / This PC)
    if (norm == QLatin1String("/")) {
        const QFileInfoList drives = QDir::drives();
        outEntries.reserve(static_cast<size_t>(drives.size()));
        for (const QFileInfo& d : drives) {
            FileEntry e;
            e.name = d.absoluteFilePath();
            e.fullPath = d.absoluteFilePath();
            e.isDir = true;
            e.permissions = 0755;
            outEntries.push_back(std::move(e));
        }
        return true;
    }
#endif

    QDir dir(norm);
    if (!dir.exists()) {
        errorOut = QStringLiteral("Directory does not exist: %1").arg(norm);
        return false;
    }

    const QFileInfoList infos = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::NoSort);
    outEntries.reserve(static_cast<size_t>(infos.size()));

    for (const QFileInfo& fi : infos) {
        FileEntry e;
        e.name = fi.fileName();
        e.fullPath = QDir::cleanPath(fi.absoluteFilePath());
        e.isDir = fi.isDir();
        e.isSymlink = fi.isSymLink();
        if (e.isSymlink) {
            e.symlinkTarget = fi.symLinkTarget();
        }
        e.size = e.isDir ? 0 : static_cast<uint64_t>(std::max<qint64>(0, fi.size()));
        e.modifiedEpoch = fi.lastModified().toSecsSinceEpoch();

        uint32_t perms = 0;
        const QFile::Permissions qp = fi.permissions();
        if (qp & QFile::ReadOwner) perms |= 0400;
        if (qp & QFile::WriteOwner) perms |= 0200;
        if (qp & QFile::ExeOwner) perms |= 0100;
        if (qp & QFile::ReadGroup) perms |= 0040;
        if (qp & QFile::WriteGroup) perms |= 0020;
        if (qp & QFile::ExeGroup) perms |= 0010;
        if (qp & QFile::ReadOther) perms |= 0004;
        if (qp & QFile::WriteOther) perms |= 0002;
        if (qp & QFile::ExeOther) perms |= 0001;
        e.permissions = perms;

        outEntries.push_back(std::move(e));
    }
    return true;
}

bool LocalFileSystem::mkdir(const QString& dirPath, QString& errorOut) {
    QDir d;
    if (!d.mkpath(dirPath)) {
        errorOut = QStringLiteral("Failed to create directory: %1").arg(dirPath);
        return false;
    }
    return true;
}

bool LocalFileSystem::renamePath(const QString& oldPath, const QString& newPath, QString& errorOut) {
    QDir d;
    if (!d.rename(oldPath, newPath)) {
        errorOut = QStringLiteral("Failed to rename '%1' to '%2'").arg(oldPath, newPath);
        return false;
    }
    return true;
}

bool LocalFileSystem::removePath(const QString& path, QString& errorOut) {
    QFileInfo fi(path);
    if (!fi.exists() && !fi.isSymLink()) return true;
    if (fi.isDir() && !fi.isSymLink()) {
        QDir d(path);
        if (!d.removeRecursively()) {
            errorOut = QStringLiteral("Failed to remove directory: %1").arg(path);
            return false;
        }
        return true;
    }
    if (!QFile::remove(path)) {
        errorOut = QStringLiteral("Failed to remove file: %1").arg(path);
        return false;
    }
    return true;
}

bool LocalFileSystem::copyFileStreaming(const QString& srcFile,
                                        const QString& dstFile,
                                        const std::function<void(uint64_t, uint64_t)>& progressCb,
                                        const std::atomic<bool>* cancelFlag,
                                        QString& errorOut) {
    QFile in(srcFile);
    if (!in.open(QIODevice::ReadOnly)) {
        errorOut = QStringLiteral("Cannot open source '%1': %2").arg(srcFile, in.errorString());
        return false;
    }
    const uint64_t total = static_cast<uint64_t>(std::max<qint64>(0, in.size()));

    QFileInfo dfi(dstFile);
    QDir().mkpath(dfi.absolutePath());
    QFile out(dstFile);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorOut = QStringLiteral("Cannot open destination '%1': %2").arg(dstFile, out.errorString());
        return false;
    }

    std::vector<char> buffer(kTransferBufferSize);
    uint64_t copied = 0;
    while (true) {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) {
            out.close();
            QFile::remove(dstFile);
            errorOut = QStringLiteral("Canceled");
            return false;
        }
        const qint64 n = in.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (n < 0) {
            errorOut = QStringLiteral("Read failed: %1").arg(in.errorString());
            return false;
        }
        if (n == 0) break;
        if (out.write(buffer.data(), n) != n) {
            errorOut = QStringLiteral("Write failed: %1").arg(out.errorString());
            return false;
        }
        copied += static_cast<uint64_t>(n);
        if (progressCb) progressCb(copied, total);
    }
    out.flush();
    out.setPermissions(in.permissions());
    if (progressCb) progressCb(total, total);
    return true;
}

}  // namespace xsync
