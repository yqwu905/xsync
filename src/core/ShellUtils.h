#pragma once

#include "core/Types.h"

#include <QString>
#include <cstdint>
#include <optional>
#include <vector>

namespace xsync {

struct RsyncProgress {
    uint64_t bytesTransferred = 0;
    int percent = 0;
    double speedBytesPerSec = 0.0;
    QString etaText;
};

class ShellUtils {
public:
    // Strict POSIX single-quote shell escaping: foo'bar -> 'foo'\''bar'
    static QString shellQuotePosix(const QString& arg);

    // Converts a local Windows/POSIX path into the format expected by the given rsync flavor.
    // On Windows, raw "C:\foo" breaks rsync because ':' denotes host:path.
    static QString toRsyncLocalPath(const QString& localPath, RsyncFlavor flavor);

    // Parses a line/chunk from `rsync --info=progress2` (e.g. "  12,345,678  45%  32.10MB/s  0:00:05")
    static std::optional<RsyncProgress> parseRsyncProgress2(const QString& text);

    // Parses OpenSSH ~/.ssh/config text into SessionProfile entries.
    static std::vector<SessionProfile> parseSshConfig(const QString& configText);

    // Expands leading ~ to user's home directory.
    static QString expandTilde(const QString& path);
};

}  // namespace xsync
