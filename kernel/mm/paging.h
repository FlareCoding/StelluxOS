#ifndef STELLUX_MM_PAGING_H
#define STELLUX_MM_PAGING_H

#include "common/types.h"
#include "paging_types.h"

namespace paging {

/**
 * @brief Get the kernel page table root (reads CR3 on x86_64, TTBR1_EL1 on aarch64).
 * @return Physical address of the kernel's top-level page table.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE pmm::phys_addr_t get_kernel_pt_root();

/**
 * @brief Set the kernel page table root (writes CR3 on x86_64, TTBR1_EL1 on aarch64).
 * @param root_pt Physical address of the new top-level page table.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_kernel_pt_root(pmm::phys_addr_t root_pt);

/**
 * @brief Convert physical address to virtual via HHDM (Higher Half Direct Map).
 * @param phys Physical address.
 * @return Virtual address in HHDM region.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void* phys_to_virt(pmm::phys_addr_t phys);

/**
 * @brief Initialize the paging subsystem. Rebuilds page tables from scratch, maps HHDM and kernel image.
 * Must be called after PMM is initialized.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Map a single page.
 * @param virt Virtual address (must be page-aligned).
 * @param phys Physical address (must be page-aligned), or PHYS_UNMAP to unmap.
 * @param flags Page flags (permissions, memory type, size).
 * @param root_pt Physical address of the page table root.
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t map_page(virt_addr_t virt, pmm::phys_addr_t phys, page_flags_t flags, pmm::phys_addr_t root_pt);

/**
 * @brief Map multiple contiguous pages.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t map_pages(virt_addr_t virt, pmm::phys_addr_t phys, page_flags_t flags, size_t count, pmm::phys_addr_t root_pt);

/**
 * @brief Unmap a single page. Idempotent - unmapping unmapped page is OK.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t unmap_page(virt_addr_t virt, pmm::phys_addr_t root_pt);

/**
 * @brief Unmap multiple contiguous pages. Idempotent.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t unmap_pages(virt_addr_t virt, size_t count, pmm::phys_addr_t root_pt);

/**
 * @brief Modify flags on an existing mapping.
 * @return OK on success, ERR_NOT_MAPPED if not mapped.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t set_page_flags(virt_addr_t virt, page_flags_t flags, pmm::phys_addr_t root_pt);

/**
 * @brief Get physical address for a virtual address.
 * @return Physical address, or 0 if not mapped.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE pmm::phys_addr_t get_physical(virt_addr_t virt, pmm::phys_addr_t root_pt);

/**
 * @brief Get page flags for a virtual address.
 * @return Page flags, or 0 if not mapped.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE page_flags_t get_page_flags(virt_addr_t virt, pmm::phys_addr_t root_pt);

/**
 * @brief Check if a virtual address is mapped.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool is_mapped(virt_addr_t virt, pmm::phys_addr_t root_pt);

/**
 * @brief TLB management - flush single page.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void flush_tlb_page(virt_addr_t virt);

/**
 * @brief TLB management - flush range.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void flush_tlb_range(virt_addr_t start, virt_addr_t end);

/**
 * @brief TLB management - flush all.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void flush_tlb_all();

/**
 * @brief Dump current mappings to serial (uses get_kernel_pt_root() internally).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dump_mappings();

/**
 * @brief Create a new page table root for user processes.
 * Copies kernel mappings as supervisor-only (Ring 3 / EL0 cannot access).
 * Lower-level tables are shared with the kernel, not duplicated.
 * @return Physical address of the new root, or 0 on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE pmm::phys_addr_t create_user_pt_root();

/**
 * @brief Destroy a user page table root.
 * Frees only the top-level table page. Does NOT free shared lower-level
 * kernel tables or user-mapped pages (caller must unmap those first).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void destroy_user_pt_root(pmm::phys_addr_t root);

/**
 * @brief Returns the value to store in task_exec_core::pt_root for a user task.
 *
 * On x86_64, there is a single CR3 register that covers the entire virtual address
 * space. The user page table (created by create_user_pt_root) already contains kernel
 * mappings in its upper half, so CR3 is set to the user page table directly.
 *
 * On aarch64, separate registers handle each half: TTBR1_EL1 for kernel addresses
 * and TTBR0_EL1 for user addresses. TTBR1 should always point to the kernel page
 * table, while TTBR0 is set per-task via user_pt_root. So pt_root stays as the
 * kernel page table root.
 *
 * @param user_pt_root Physical address of the user page table (from load_elf).
 * @return Physical address to store in task_exec_core::pt_root.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE pmm::phys_addr_t supervisor_pt_root_for_user_task(pmm::phys_addr_t user_pt_root);

} // namespace paging

#endif // STELLUX_MM_PAGING_H
