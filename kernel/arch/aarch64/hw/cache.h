#ifndef STELLUX_ARCH_AARCH64_HW_CACHE_H
#define STELLUX_ARCH_AARCH64_HW_CACHE_H

#include "common/types.h"

namespace cache {

constexpr size_t CACHE_LINE_SIZE = 64;

/**
 * Ensure stores to a memory range are visible to instruction fetch.
 * Required on AArch64 after writing instructions via data stores
 * (e.g., ELF loading, JIT, self-modifying code).
 *
 * Cleans D-cache to PoU and invalidates I-cache for the range.
 */
inline void flush_icache_range(uintptr_t start, size_t size) {
    if (size == 0) return;

    uintptr_t end = start + size;
    uintptr_t addr = start & ~(CACHE_LINE_SIZE - 1);

    for (; addr < end; addr += CACHE_LINE_SIZE) {
        asm volatile("dc cvau, %0" :: "r"(addr) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");

    addr = start & ~(CACHE_LINE_SIZE - 1);
    for (; addr < end; addr += CACHE_LINE_SIZE) {
        asm volatile("ic ivau, %0" :: "r"(addr) : "memory");
    }

    asm volatile("dsb ish\n" "isb" ::: "memory");
}

} // namespace cache

#endif // STELLUX_ARCH_AARCH64_HW_CACHE_H
