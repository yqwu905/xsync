#pragma once

#include "core/Types.h"

#include <QString>
#include <atomic>
#include <vector>

namespace xsync {

class SshClient;

// Local filesystem helper functions mirroring SshClient's directory/file operations
class LocalFileSystem {
public:
    static QString defaultHomePath();
    static QString normalizePath(const QString& path);
    static QString parentPath(const QString& path);
    static QString joinPath(const QString& dir, const QString& name);

    static bool listDirectory(const QString& dirPath,
                              std::vector<FileEntry>& outEntries,
                              QString& errorOut);
    static bool mkdir(const QString& dirPath, QString& errorOut);
    static bool renamePath(const QString& oldPath, const QString& newPath, QString& errorOut);
    static bool removePath(const QString& path, QString& errorOut);

    // Streaming file copy using fixed 256 KiB buffer with progress + cancel
    static bool copyFileStreaming(const QString& srcFile,
                                  const QString& dstFile,
                                  const std::function<void(uint64_t, uint64_t)>& progressCb,
                                  const std::atomic<bool>* cancelFlag,
                                  QString& errorOut);
};

}  // namespace xsync
