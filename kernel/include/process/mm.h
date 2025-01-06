#ifndef MM_H
#define MM_H
#include <types.h>

struct mm_context {
    uint64_t root_page_table;
};

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

#endif // MM_H
