#ifndef STELLUX_MM_PMM_INTERNAL_H
#define STELLUX_MM_PMM_INTERNAL_H

#include "common/types.h"
#include "pmm_types.h"

namespace pmm {

constexpr phys_addr_t ZONE_DMA32_LIMIT = 0x100000000ULL;
constexpr uint32_t PERCPU_CACHE_SIZE = 64;

struct free_area {
    pfn_t    first;
    pfn_t    last;
    uint64_t count;

    bool empty() const { return first == INVALID_PFN; }
};

struct zone {
    const char* name;
    pfn_t       start_pfn;
    pfn_t       end_pfn;
    uint64_t    total_pages;
    uint64_t    free_pages;

    free_area   free_areas[MAX_ORDER + 1];
};

struct pmm_state {
    bool                    initialized;
    pfn_t                   max_pfn;          // Highest valid PFN + 1
    page_frame_descriptor*  page_array;       // Virtual address
    phys_addr_t             page_array_phys;  // Physical address of page_array
    size_t                  page_array_size;  // Size in bytes
    zone                    zones[static_cast<size_t>(zone_id::COUNT)];
};

extern pmm_state g_pmm;

inline zone_id get_zone_for_pfn(pfn_t pfn) {
    phys_addr_t addr = pfn_to_phys(pfn);
    if (addr < ZONE_DMA32_LIMIT) {
        return zone_id::DMA32;
    }
    return zone_id::NORMAL;
}

constexpr pfn_t buddy_pfn(pfn_t pfn, uint8_t order) {
    return pfn ^ (static_cast<pfn_t>(1) << order);
}

/**
 * Add a free block to the zone's freelist at the specified order.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void freelist_add(zone& z, pfn_t pfn, uint8_t order);

/**
 * Remove a block from the zone's freelist at the specified order.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void freelist_remove(zone& z, pfn_t pfn, uint8_t order);

/**
 * Allocate from a specific zone.
 * @return Physical address or 0 on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE phys_addr_t zone_alloc(zone& z, uint8_t order);

/**
 * Free to a specific zone with buddy coalescing.
 * @return OK or error code.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t zone_free(zone& z, pfn_t pfn, uint8_t order);

} // namespace pmm

#endif // STELLUX_MM_PMM_INTERNAL_H
