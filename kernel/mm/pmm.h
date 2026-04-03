#ifndef STELLUX_MM_PMM_H
#define STELLUX_MM_PMM_H

#include "pmm_types.h"

namespace pmm {

/**
 * @brief Early page allocator for page tables before our own HHDM is ready.
 * Uses bump allocation from a large USABLE region that Limine reliably maps.
 */
class bootstrap_allocator {
public:
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static int32_t init();
    
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static phys_addr_t alloc_page();
    
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static phys_addr_t get_region_start();
    
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static phys_addr_t get_used_end();
    
    /**
     * @brief Check if bootstrap allocator is initialized and has capacity.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static bool is_active();
    
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static size_t get_pages_allocated();
    
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static size_t get_pages_remaining();
};

/**
 * @brief Initialize the physical memory manager.
 * Must be called after boot services are available.
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Allocate 2^order contiguous pages.
 * @param order Number of pages = 2^order (0=1 page, 1=2 pages, ..., 10=1024 pages)
 * @param zones Which zones to allocate from (ZONE_DMA32, ZONE_NORMAL, ZONE_ANY)
 * @return Physical address of first page, or 0 on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE phys_addr_t alloc_pages(uint8_t order, zone_mask_t zones = ZONE_ANY);

/**
 * @brief Free 2^order contiguous pages.
 * @param addr Physical address (must be page-aligned)
 * @param order Same order used for allocation
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t free_pages(phys_addr_t addr, uint8_t order);

/**
 * @brief Allocate a single page (convenience wrapper for alloc_pages(0, zones)).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE phys_addr_t alloc_page(zone_mask_t zones = ZONE_ANY);

/**
 * @brief Free a single page (convenience wrapper for free_pages(addr, 0)).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t free_page(phys_addr_t addr);

/**
 * @brief Get the page frame descriptor for a physical address.
 * @return nullptr if address is out of range.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE page_frame_descriptor* get_page_frame(phys_addr_t addr);

/**
 * @brief Get total free pages across specified zones.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint64_t free_page_count(zone_mask_t zones = ZONE_ANY);

/**
 * @brief Get free blocks at a specific order in specified zones.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint64_t free_block_count(uint8_t order, zone_mask_t zones = ZONE_ANY);

/**
 * @brief Dump PMM statistics to serial (for debugging).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dump_stats();

} // namespace pmm

#endif // STELLUX_MM_PMM_H
