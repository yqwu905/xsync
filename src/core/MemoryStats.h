#pragma once

#include <cstdint>

namespace xsync {

class MemoryStats {
public:
    // Current resident memory (RSS on Linux / Working Set on Windows) in bytes.
    static uint64_t currentRssBytes();
    static double currentRssMegabytes();

    // Ask OS allocator to return free heap pages (malloc_trim on glibc,
    // EmptyWorkingSet / SetProcessWorkingSetSize on Windows).
    static void trimProcessMemory();
};

}  // namespace xsync
