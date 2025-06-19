#ifndef MM_H
#define MM_H
#include <types.h>

// Forward declaration of vma_area
struct vma_area;

struct mm_context {
    uint64_t root_page_table;
    
    // VMA management
    vma_area* vma_list;           // Head of the VMA list
    uintptr_t mmap_base;          // Base address for mmap allocations
    uintptr_t task_size;          // Size of the task's address space
    size_t vma_count;             // Number of VMAs

    // Process heap
    uintptr_t heap_start;         // Initial brk_end
    uintptr_t heap_end;           // brk_end
} __attribute__((packed));

/**
 * @brief Reads the current memory management context.
 * 
 * Retrieves the root page table address (or equivalent) currently installed in the MMU.
 * This function typically reads the CR3 register or its platform-specific equivalent.
 * 
 * @return mm_context The current memory management context.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE mm_context save_mm_context();

/**
 * @brief Installs a memory management context into the MMU.
 * 
 * Updates the MMU to use the given memory management context. This function
 * typically writes to the CR3 register or its platform-specific equivalent to
 * switch the page tables.
 * 
 * @param context The memory management context to install.
 * @note Privilege: **required**
 */
void install_mm_context(const mm_context& context);

/**
 * @brief Grows or shrinks the process heap.
 * 
 * Updates the heap end address by either allocating new pages (growth) or
 * freeing existing pages (shrinkage). Uses VMA management to track heap regions.
 * 
 * @param mm_ctx The process's memory management context
 * @param new_heap_end The new heap end address (page-aligned)
 * @return true if the operation was successful, false otherwise
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool manage_process_heap(mm_context* mm_ctx, uintptr_t new_heap_end);

#endif // MM_H
