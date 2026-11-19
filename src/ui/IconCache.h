#pragma once

#include "core/Types.h"

#include <QIcon>

namespace xsync {

// Fixed-size static icon cache backed by compiled-in SVGs.
// Avoids QFileIconProvider / Win32 Shell COM icon handle leaks over long sessions.
class IconCache {
public:
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

    static const QIcon& iconForEntry(const FileEntry& entry);
};

}  // namespace xsync
