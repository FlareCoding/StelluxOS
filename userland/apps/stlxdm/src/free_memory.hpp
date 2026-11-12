#ifndef STLXDM_FREE_MEMORY_HPP
#define STLXDM_FREE_MEMORY_HPP

#include <cstdint>

/* Free memory as the kernel counts it, in bytes, or UINT64_MAX where it cannot say */
using free_memory_source = uint64_t (*)();
uint64_t kernel_free_memory();

/* Whether `bytes` more fit in free memory with 16 MB to spare, as `source` counts it. The kernel hands a page out
 * only when it is first touched and kills the toucher when none is left, so stlxdm asks before its large allocations.
 * This narrows that problem rather than solving it: memory another process takes after the check still faults. */
bool has_memory_for(uint64_t bytes, free_memory_source source = kernel_free_memory);

#endif
