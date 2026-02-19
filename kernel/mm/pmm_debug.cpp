#include "mm/pmm.h"
#include "mm/pmm_internal.h"
#include "common/logging.h"

namespace pmm {

// Size unit constants
constexpr uint64_t KB = 1024;
constexpr uint64_t MB = 1024 * KB;
constexpr uint64_t GB = 1024 * MB;

// Helper to format bytes as human-readable string
// Uses MB for precision unless value is >= 8GB
static const char* format_size(uint64_t bytes, uint64_t& out_value) {
    if (bytes >= 8 * GB) {
        out_value = bytes / GB;
        return "GB";
    } else if (bytes >= MB) {
        out_value = bytes / MB;
        return "MB";
    } else if (bytes >= KB) {
        out_value = bytes / KB;
        return "KB";
    } else {
        out_value = bytes;
        return "B";
    }
}

__PRIVILEGED_CODE void dump_stats() {
    if (!g_pmm.initialized) {
        log::info("PMM: not initialized");
        return;
    }

    log::info("PMM Statistics:");
    uint64_t arr_size_val;
    const char* arr_size_unit = format_size(g_pmm.page_array_size, arr_size_val);
    log::info("  page_array: phys=0x%lx size=0x%lx (%lu %s)",
              g_pmm.page_array_phys, g_pmm.page_array_size,
              arr_size_val, arr_size_unit);

    uint64_t total_free = 0;
    uint64_t total_pages = 0;

    for (size_t zi = 0; zi < static_cast<size_t>(zone_id::COUNT); zi++) {
        const zone& z = g_pmm.zones[zi];

        // Skip empty zones
        if (z.end_pfn <= z.start_pfn) {
            log::info("  %s zone: empty", z.name);
            continue;
        }

        // Zone address range
        phys_addr_t zone_start = pfn_to_phys(z.start_pfn);
        phys_addr_t zone_end = pfn_to_phys(z.end_pfn);
        uint64_t zone_span = zone_end - zone_start;
        uint64_t zone_span_val;
        const char* zone_span_unit = format_size(zone_span, zone_span_val);

        uint64_t free_bytes = z.free_pages * PAGE_SIZE;
        uint64_t total_bytes = z.total_pages * PAGE_SIZE;
        uint64_t free_val, total_val;
        const char* mem_unit = format_size(total_bytes, total_val);
        format_size(free_bytes, free_val);

        log::info("  %s zone: 0x%lx - 0x%lx (spans %lu %s)",
                  z.name, zone_start, zone_end, zone_span_val, zone_span_unit);
        log::info("    %lu/%lu pages free (%lu/%lu %s)",
                  z.free_pages, z.total_pages,
                  free_val, total_val, mem_unit);

        // Show free blocks per order
        for (uint8_t o = 0; o <= MAX_ORDER; o++) {
            if (z.free_areas[o].count > 0) {
                uint64_t block_size = order_to_pages(o) * PAGE_SIZE;
                uint64_t block_val;
                const char* block_unit = format_size(block_size, block_val);
                log::info("    order %2u (%lu %s): %lu blocks",
                          o, block_val, block_unit, z.free_areas[o].count);
            }
        }

        total_free += z.free_pages;
        total_pages += z.total_pages;
    }

    uint64_t total_free_bytes = total_free * PAGE_SIZE;
    uint64_t total_bytes = total_pages * PAGE_SIZE;

    uint64_t total_free_val, total_val;
    const char* total_unit = format_size(total_bytes, total_val);
    format_size(total_free_bytes, total_free_val);

    log::info("  Total: %lu/%lu pages free (%lu/%lu %s)",
              total_free, total_pages,
              total_free_val, total_val, total_unit);
}

#ifdef DEBUG
// Validate freelist integrity (debug builds only)
__PRIVILEGED_CODE bool validate_freelists() {
    if (!g_pmm.initialized) return false;

    bool valid = true;

    for (size_t zi = 0; zi < static_cast<size_t>(zone_id::COUNT); zi++) {
        const zone& z = g_pmm.zones[zi];
        if (z.end_pfn <= z.start_pfn) continue;

        for (uint8_t order = 0; order <= MAX_ORDER; order++) {
            const free_area& fa = z.free_areas[order];
            uint64_t counted = 0;

            pfn_t pfn = fa.first;
            pfn_t prev = INVALID_PFN;

            while (pfn != INVALID_PFN) {
                // Check bounds
                if (pfn >= g_pmm.max_pfn) {
                    log::error("PMM validate: pfn 0x%x out of bounds in %s order %u",
                               pfn, z.name, order);
                    valid = false;
                    break;
                }

                const page_frame_descriptor& pf = g_pmm.page_array[pfn];

                // Check it's actually free
                if (!pf.is_free()) {
                    log::error("PMM validate: pfn 0x%x not free in freelist %s order %u",
                               pfn, z.name, order);
                    valid = false;
                }

                // Check order matches
                if (pf.order != order) {
                    log::error("PMM validate: pfn 0x%x order mismatch (%u vs %u) in %s",
                               pfn, pf.order, order, z.name);
                    valid = false;
                }

                // Check prev pointer
                if (pf.list_prev != prev) {
                    log::error("PMM validate: pfn 0x%x bad prev pointer in %s order %u",
                               pfn, z.name, order);
                    valid = false;
                }

                prev = pfn;
                pfn = pf.list_next;
                counted++;

                // Sanity check to avoid infinite loops
                if (counted > g_pmm.max_pfn) {
                    log::error("PMM validate: infinite loop in %s order %u",
                               z.name, order);
                    valid = false;
                    break;
                }
            }

            // Check count matches
            if (counted != fa.count) {
                log::error("PMM validate: count mismatch in %s order %u: counted %lu, stored %lu",
                           z.name, order, counted, fa.count);
                valid = false;
            }

            // Check last pointer
            if (fa.count > 0 && fa.last != prev) {
                log::error("PMM validate: bad last pointer in %s order %u",
                           z.name, order);
                valid = false;
            }
        }
    }

    if (valid) {
        log::debug("PMM validate: all freelists valid");
    }

    return valid;
}
#endif // DEBUG

} // namespace pmm
