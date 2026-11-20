#include "ui/IconCache.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QSet>
#include <QSvgRenderer>
#include <algorithm>
#include <cmath>

static void initXsyncQtResources() {
    static bool done = false;
    if (!done) {
        Q_INIT_RESOURCE(xsync);
        done = true;
    }
}

namespace xsync {

namespace {

bool g_darkTheme = true;

QHash<QString, QPixmap>& themedPixmapCache() {
    static QHash<QString, QPixmap> cache;
    static bool postRoutineRegistered = false;
    if (!postRoutineRegistered) {
        postRoutineRegistered = true;
        qAddPostRoutine([]() { themedPixmapCache().clear(); });
    }
    return cache;
}

class ThemedSvgIconEngine final : public QIconEngine {
public:
    explicit ThemedSvgIconEngine(const QString& resourcePath, bool accentStar = false)
        : resourcePath_(resourcePath), accentStar_(accentStar) {
        initXsyncQtResources();
        QFile f(resourcePath_);
        if (f.open(QIODevice::ReadOnly)) {
            svgTemplate_ = f.readAll();
        }
    }

    bool isNull() override { return svgTemplate_.isEmpty(); }

    QIconEngine* clone() const override {
        return new ThemedSvgIconEngine(*this);
    }

    QSize actualSize(const QSize& size, QIcon::Mode /*mode*/, QIcon::State /*state*/) override {
        return size;
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State /*state*/, qreal scale) override {
        if (svgTemplate_.isEmpty() || !size.isValid() || size.width() <= 0 || size.height() <= 0) {
            return {};
        }

        const qreal dpr = (scale > 0.0) ? scale : 1.0;
        const int pxW = std::max(1, static_cast<int>(std::lround(size.width() * dpr)));
        const int pxH = std::max(1, static_cast<int>(std::lround(size.height() * dpr)));
        const QByteArray colorHex = colorHexFor(mode);

        const QString cacheKey = QStringLiteral("%1|%2|%3x%4|%5")
                                     .arg(resourcePath_, QString::fromLatin1(colorHex))
                                     .arg(pxW)
                                     .arg(pxH)
                                     .arg(static_cast<int>(std::lround(dpr * 100.0)));

        auto& cache = themedPixmapCache();
        if (auto it = cache.constFind(cacheKey); it != cache.constEnd()) {
            return it.value();
        }

        // Keep cache strictly bounded (zero memory growth over long sessions)
        if (cache.size() >= 256) {
            cache.clear();
        }

        QByteArray coloredSvg = svgTemplate_;
        coloredSvg.replace("currentColor", colorHex);

        QPixmap pix(pxW, pxH);
        pix.fill(Qt::transparent);
        {
            QPainter p(&pix);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            QSvgRenderer renderer(coloredSvg);
            renderer.render(&p, QRectF(0.0, 0.0, static_cast<qreal>(pxW), static_cast<qreal>(pxH)));
        }
        pix.setDevicePixelRatio(dpr);
        cache.insert(cacheKey, pix);
        return pix;
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        if (!painter || !rect.isValid() || svgTemplate_.isEmpty()) return;
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        const QPixmap pix = scaledPixmap(rect.size(), mode, state, dpr);
        if (!pix.isNull()) {
            painter->drawPixmap(rect, pix);
        }
    }

private:
    QByteArray colorHexFor(QIcon::Mode mode) const {
        if (accentStar_) {
            if (mode == QIcon::Disabled) {
                return g_darkTheme ? QByteArrayLiteral("#64748b") : QByteArrayLiteral("#94a3b8");
            }
            return g_darkTheme ? QByteArrayLiteral("#fbbf24") : QByteArrayLiteral("#d97706");
        }
        switch (mode) {
            case QIcon::Disabled:
                return g_darkTheme ? QByteArrayLiteral("#475569") : QByteArrayLiteral("#94a3b8");
            case QIcon::Selected:
                return QByteArrayLiteral("#ffffff");
            case QIcon::Active:
                return g_darkTheme ? QByteArrayLiteral("#ffffff") : QByteArrayLiteral("#0f172a");
            case QIcon::Normal:
            default:
                return g_darkTheme ? QByteArrayLiteral("#cbd5e1") : QByteArrayLiteral("#334155");
        }
    }

    QString resourcePath_;
    QByteArray svgTemplate_;
    bool accentStar_ = false;
};

QIcon makeThemedIcon(const QString& resourcePath, bool accentStar = false) {
    initXsyncQtResources();
    return QIcon(new ThemedSvgIconEngine(resourcePath, accentStar));
}

}  // namespace

void IconCache::setDarkTheme(bool dark) {
    if (g_darkTheme != dark) {
        g_darkTheme = dark;
        themedPixmapCache().clear();
    }
}

bool IconCache::isDarkTheme() {
    return g_darkTheme;
}

const QIcon& IconCache::appIcon() {
    initXsyncQtResources();
    static const QIcon ic(QStringLiteral(":/icons/app.svg"));
    return ic;
}

const QIcon& IconCache::folderIcon() {
    initXsyncQtResources();
    static const QIcon ic(QStringLiteral(":/icons/folder.svg"));
    return ic;
}

const QIcon& IconCache::folderUpIcon() {
    initXsyncQtResources();
    static const QIcon ic(QStringLiteral(":/icons/folder_up.svg"));
    return ic;
}

const QIcon& IconCache::fileIcon() {
    initXsyncQtResources();
    static const QIcon ic(QStringLiteral(":/icons/file.svg"));
    return ic;
}

const QIcon& IconCache::codeFileIcon() {
    initXsyncQtResources();
    static const QIcon ic(QStringLiteral(":/icons/file_code.svg"));
    return ic;
}

const QIcon& IconCache::archiveFileIcon() {
    initXsyncQtResources();
    static const QIcon ic(QStringLiteral(":/icons/file_archive.svg"));
    return ic;
}

const QIcon& IconCache::imageFileIcon() {
    initXsyncQtResources();
    static const QIcon ic(QStringLiteral(":/icons/file_image.svg"));
    return ic;
}

const QIcon& IconCache::symlinkIcon() {
    initXsyncQtResources();
    static const QIcon ic(QStringLiteral(":/icons/symlink.svg"));
    return ic;
}

const QIcon& IconCache::serverIcon() {
    initXsyncQtResources();
    static const QIcon ic(QStringLiteral(":/icons/server.svg"));
    return ic;
}

const QIcon& IconCache::computerIcon() {
    initXsyncQtResources();
    static const QIcon ic(QStringLiteral(":/icons/computer.svg"));
    return ic;
}

const QIcon& IconCache::arrowLeftIcon() {
    static const QIcon ic = makeThemedIcon(QStringLiteral(":/icons/ui/arrow_left.svg"));
    return ic;
}

const QIcon& IconCache::arrowUpIcon() {
    static const QIcon ic = makeThemedIcon(QStringLiteral(":/icons/ui/arrow_up.svg"));
    return ic;
}

const QIcon& IconCache::arrowDownIcon() {
    static const QIcon ic = makeThemedIcon(QStringLiteral(":/icons/ui/arrow_down.svg"));
    return ic;
}

const QIcon& IconCache::homeIcon() {
    static const QIcon ic = makeThemedIcon(QStringLiteral(":/icons/ui/home.svg"));
    return ic;
}

const QIcon& IconCache::refreshIcon() {
    static const QIcon ic = makeThemedIcon(QStringLiteral(":/icons/ui/refresh.svg"));
    return ic;
}

const QIcon& IconCache::transferIcon() {
    static const QIcon ic = makeThemedIcon(QStringLiteral(":/icons/ui/transfer.svg"));
    return ic;
}

const QIcon& IconCache::newWindowIcon() {
    static const QIcon ic = makeThemedIcon(QStringLiteral(":/icons/ui/window_new.svg"));
    return ic;
}

const QIcon& IconCache::plusIcon() {
    static const QIcon ic = makeThemedIcon(QStringLiteral(":/icons/ui/plus.svg"));
    return ic;
}

const QIcon& IconCache::starOutlineIcon() {
    static const QIcon ic = makeThemedIcon(QStringLiteral(":/icons/ui/star_outline.svg"));
    return ic;
}

const QIcon& IconCache::starFilledIcon() {
    static const QIcon ic = makeThemedIcon(QStringLiteral(":/icons/ui/star_filled.svg"), true);
    return ic;
}

const QIcon& IconCache::bookmarksIcon() {
    static const QIcon ic = makeThemedIcon(QStringLiteral(":/icons/ui/bookmarks.svg"));
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
