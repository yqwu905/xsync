#pragma once

#include "core/Types.h"

#include <QIcon>

namespace xsync {

// Fixed-size static icon cache backed by compiled-in SVGs.
// Avoids QFileIconProvider / Win32 Shell COM icon handle leaks over long sessions.
// Monochrome action icons use ThemedSvgIconEngine for crisp HiDPI rendering and
// instant automatic Dark/Light recoloring.
class IconCache {
public:
    static void setDarkTheme(bool dark);
    static bool isDarkTheme();

    // Multi-color app / file / host icons
    static const QIcon& appIcon();
    static const QIcon& folderIcon();
    static const QIcon& folderUpIcon();
    static const QIcon& fileIcon();
    static const QIcon& codeFileIcon();
    static const QIcon& archiveFileIcon();
    static const QIcon& imageFileIcon();
    static const QIcon& symlinkIcon();
    static const QIcon& serverIcon();
    static const QIcon& computerIcon();

    // Monochrome themed UI action icons
    static const QIcon& arrowLeftIcon();
    static const QIcon& arrowUpIcon();
    static const QIcon& arrowDownIcon();
    static const QIcon& homeIcon();
    static const QIcon& refreshIcon();
    static const QIcon& transferIcon();
    static const QIcon& newWindowIcon();
    static const QIcon& plusIcon();
    static const QIcon& starOutlineIcon();
    static const QIcon& starFilledIcon();
    static const QIcon& bookmarksIcon();

    static const QIcon& iconForEntry(const FileEntry& entry);
};

}  // namespace xsync
