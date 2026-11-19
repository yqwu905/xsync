#include "core/MemoryStats.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace xsync {

uint64_t MemoryStats::currentRssBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return static_cast<uint64_t>(pmc.WorkingSetSize);
    }
    return 0;
#elif defined(__linux__)
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long sizePages = 0;
    unsigned long rssPages = 0;
    int scanned = std::fscanf(f, "%lu %lu", &sizePages, &rssPages);
    std::fclose(f);
    if (scanned != 2) return 0;
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    return static_cast<uint64_t>(rssPages) * static_cast<uint64_t>(pageSize > 0 ? pageSize : 4096);
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<uint64_t>(info.resident_size);
    }
    return 0;
#else
    return 0;
#endif
}

double MemoryStats::currentRssMegabytes() {
    return static_cast<double>(currentRssBytes()) / (1024.0 * 1024.0);
}

void MemoryStats::trimProcessMemory() {
#ifdef _WIN32
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
#elif defined(__linux__) && defined(__GLIBC__)
    malloc_trim(0);
#endif
}

}  // namespace xsync
