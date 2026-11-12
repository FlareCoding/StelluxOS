#include "free_memory.hpp"

#if defined(__linux__)
#include <sys/sysinfo.h>
#endif

constexpr uint64_t RESERVE_BYTES = 16ull << 20;

uint64_t kernel_free_memory() {
#if defined(__linux__)
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        return static_cast<uint64_t>(info.freeram) * info.mem_unit;
    }
#endif

    return UINT64_MAX;
}

bool has_memory_for(uint64_t bytes, free_memory_source source) {
    uint64_t free_bytes = source();
    return free_bytes >= RESERVE_BYTES && free_bytes - RESERVE_BYTES >= bytes;
}
