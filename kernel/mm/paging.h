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

} // namespace paging

#endif // STELLUX_MM_PAGING_H
