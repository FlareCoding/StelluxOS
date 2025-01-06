#ifdef ARCH_X86_64
#include <process/mm.h>
#include <memory/memory.h>
#include <memory/paging.h>

__PRIVILEGED_CODE
mm_context save_mm_context() {
    mm_context ctx;
    zeromem(&ctx, sizeof(mm_context));

    ctx.root_page_table = reinterpret_cast<uint64_t>(paging::get_pml4());

    return ctx;
}

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
__PRIVILEGED_CODE
void install_mm_context(const mm_context& context) {
    paging::set_pml4(reinterpret_cast<paging::page_table*>(context.root_page_table));
}
#endif // ARCH_X86_64

