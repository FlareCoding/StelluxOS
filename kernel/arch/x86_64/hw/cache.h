#ifndef STELLUX_ARCH_X86_64_HW_CACHE_H
#define STELLUX_ARCH_X86_64_HW_CACHE_H

#include "common/types.h"

namespace cache {

/**
 * No-op on x86_64: instruction and data caches are coherent.
 */
inline void flush_icache_range(uintptr_t, size_t) {}

/**
 * No-op on x86_64: cache is snooped by DMA, and UC mappings bypass cache.
 */
inline void clean_dcache_poc(uintptr_t, size_t) {}

} // namespace cache

#endif // STELLUX_ARCH_X86_64_HW_CACHE_H
