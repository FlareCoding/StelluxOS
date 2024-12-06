#ifndef TLB_H
#define TLB_H
#include <types.h>

namespace paging {
/**
 * @brief Invalidates the TLB entry for a specific virtual address.
 * 
 * This function uses the `invlpg` instruction to invalidate the TLB entry corresponding
 * to the provided virtual address. This is useful when a single page's mapping changes
 * and you need to ensure that the CPU fetches the updated mapping.
 * 
 * @param addr The virtual address of the page to invalidate in the TLB.
 */
__PRIVILEGED_CODE static inline void invlpg(void* vaddr) {
    asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

/**
 * @brief Flushes the entire TLB.
 * 
 * **Note**: Ensure that reloading `CR3` with the current value is safe in your context.
 */
__PRIVILEGED_CODE static inline void tlb_flush_all() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
} // namespace paging

#endif // TLB_H
