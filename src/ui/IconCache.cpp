#include "ui/IconCache.h"

#include <QSet>

static void initXsyncQtResources() {
    static bool done = false;
    if (!done) {
        Q_INIT_RESOURCE(xsync);
        done = true;
    }
}

namespace xsync {

const QIcon& IconCache::appIcon() {
    initXsyncQtResources();
    static const QIcon ic(QStringLiteral(":/icons/app.svg"));
    return ic;
}

const QIcon& IconCache::folderIcon() {
    static const QIcon ic(QStringLiteral(":/icons/folder.svg"));
    return ic;
}

const QIcon& IconCache::folderUpIcon() {
    static const QIcon ic(QStringLiteral(":/icons/folder_up.svg"));
    return ic;
}

const QIcon& IconCache::fileIcon() {
    static const QIcon ic(QStringLiteral(":/icons/file.svg"));
    return ic;
}

const QIcon& IconCache::codeFileIcon() {
    static const QIcon ic(QStringLiteral(":/icons/file_code.svg"));
    return ic;
}

const QIcon& IconCache::archiveFileIcon() {
    static const QIcon ic(QStringLiteral(":/icons/file_archive.svg"));
    return ic;
}

const QIcon& IconCache::imageFileIcon() {
    static const QIcon ic(QStringLiteral(":/icons/file_image.svg"));
    return ic;
}

const QIcon& IconCache::symlinkIcon() {
    static const QIcon ic(QStringLiteral(":/icons/symlink.svg"));
    return ic;
}

const QIcon& IconCache::serverIcon() {
    static const QIcon ic(QStringLiteral(":/icons/server.svg"));
    return ic;
}

const QIcon& IconCache::computerIcon() {
    static const QIcon ic(QStringLiteral(":/icons/computer.svg"));
    return ic;
}

const QIcon& IconCache::iconForEntry(const FileEntry& entry) {
    if (entry.isParentLink) return folderUpIcon();
    if (entry.isDir) return folderIcon();
    if (entry.isSymlink) return symlinkIcon();

    const int dot = entry.name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0 && dot + 1 < entry.name.size()) {
        const QString ext = entry.name.mid(dot + 1).toLower();
        static const QSet<QString> kCodeExts = {
            QStringLiteral("c"), QStringLiteral("cc"), QStringLiteral("cpp"), QStringLiteral("h"),
            QStringLiteral("hpp"), QStringLiteral("py"), QStringLiteral("rs"), QStringLiteral("go"),
            QStringLiteral("js"), QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("jsx"),
            QStringLiteral("java"), QStringLiteral("sh"), QStringLiteral("ps1"), QStringLiteral("bat"),
            QStringLiteral("json"), QStringLiteral("yaml"), QStringLiteral("yml"), QStringLiteral("toml"),
            QStringLiteral("xml"), QStringLiteral("cmake"), QStringLiteral("md"), QStringLiteral("sql"),
            QStringLiteral("html"), QStringLiteral("css")};
        static const QSet<QString> kArchiveExts = {
            QStringLiteral("zip"), QStringLiteral("tar"), QStringLiteral("gz"), QStringLiteral("tgz"),
            QStringLiteral("bz2"), QStringLiteral("xz"), QStringLiteral("7z"), QStringLiteral("rar"),
            QStringLiteral("zst"), QStringLiteral("deb"), QStringLiteral("rpm")};
        static const QSet<QString> kImageExts = {
            QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"),
            QStringLiteral("bmp"), QStringLiteral("webp"), QStringLiteral("svg"), QStringLiteral("ico"),
            QStringLiteral("tif"), QStringLiteral("tiff")};

        if (kCodeExts.contains(ext)) return codeFileIcon();
        if (kArchiveExts.contains(ext)) return archiveFileIcon();
        if (kImageExts.contains(ext)) return imageFileIcon();
    }
    return fileIcon();
}

}  // namespace xsync
