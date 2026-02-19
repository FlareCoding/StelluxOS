#include "mm/pmm.h"
#include "mm/pmm_internal.h"
#include "mm/paging.h"
#include "boot/boot_services.h"
#include "common/utils/logging.h"
#include "common/utils/memory.h"

// Linker symbols for kernel boundaries
extern "C" {
    extern char __stlx_kern_start[];
    extern char __stlx_kern_end[];
}

namespace pmm {

// Bootstrap allocator state
__PRIVILEGED_DATA static struct {
    phys_addr_t region_start;   // Start of bootstrap region (page-aligned)
    phys_addr_t region_end;     // End of bootstrap region (page-aligned)
    phys_addr_t current;        // Next page to allocate (bump pointer)
    size_t pages_allocated;     // Counter for statistics
    bool initialized;
} g_bootstrap_info = {};

// Compute exact page table requirement for 4-level paging
// Given N pages to map: L3 = ceil(N/512), L2 = ceil(L3/512), L1 = ceil(L2/512), L0 = 1
__PRIVILEGED_CODE static uint64_t calc_page_tables_for_pages(uint64_t page_count) {
    uint64_t l3 = (page_count + 511) / 512;   // Each L3 covers 512 pages (2MB)
    uint64_t l2 = (l3 + 511) / 512;           // Each L2 covers 512 L3s (1GB)
    uint64_t l1 = (l2 + 511) / 512;           // Each L1 covers 512 L2s (512GB)
    uint64_t l0 = 1;                          // Root table
    return l0 + l1 + l2 + l3;
}

// Compute page tables needed for initial mappings (HHDM + kernel + devices)
__PRIVILEGED_CODE static size_t compute_required_pages() {
    // HHDM mapping: use full physical address span (0 to max_phys)
    // Gaps in physical memory still affect which L1/L2 indices are used,
    // so we must calculate based on the span, not just usable page count
    phys_addr_t max_phys = 0;
    for (uint64_t i = 0; i < g_boot_info.memmap_entry_count; i++) {
        auto* e = g_boot_info.memmap_entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            phys_addr_t end = page_align_down(e->base + e->length);
            if (end > max_phys) max_phys = end;
        }
    }
    uint64_t hhdm_tables = calc_page_tables_for_pages(max_phys / PAGE_SIZE);
    
    // Kernel mapping: exact size from linker symbols
    // Kernel is at 0xffffffff80000000 (different L0 index than HHDM)
    // Needs its own L1/L2/L3 chain (L0 root table is shared)
    uint64_t kernel_size = reinterpret_cast<uint64_t>(__stlx_kern_end) - 
                           reinterpret_cast<uint64_t>(__stlx_kern_start);
    uint64_t kernel_pages = (kernel_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t kernel_l3 = (kernel_pages + 511) / 512;
    uint64_t kernel_tables = 1 + 1 + kernel_l3;  // L1 + L2 + L3s
    
    // Device mappings during paging::init()
    // - aarch64: UART at 0x09000000 needs mapping (1 page, may need L1+L2+L3 tables)
    // - x86_64: serial uses port I/O, no MMIO mapping needed
#if defined(__aarch64__)
    uint64_t device_tables = 3;
#else
    uint64_t device_tables = 0;
#endif
    
    return static_cast<size_t>(hhdm_tables + kernel_tables + device_tables);
}

// Find suitable USABLE region. Prefers lower memory (< 4GB) and larger regions.
__PRIVILEGED_CODE static bool find_suitable_region(size_t required_pages, phys_addr_t& out_start, phys_addr_t& out_end) {
    size_t required_size = required_pages * PAGE_SIZE;
    phys_addr_t best_start = 0;
    phys_addr_t best_end = 0;
    size_t best_size = 0;
    bool best_is_low = false;
    bool found = false;
    
    for (uint64_t i = 0; i < g_boot_info.memmap_entry_count; i++) {
        auto* e = g_boot_info.memmap_entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        
        phys_addr_t start = page_align_up(e->base);
        phys_addr_t end = page_align_down(e->base + e->length);
        
        if (end <= start) continue;
        size_t size = end - start;
        if (size < required_size) continue;
        
        bool is_low = (end <= 0x100000000ULL);
        bool is_better = !found || 
                         (is_low && !best_is_low) || 
                         (is_low == best_is_low && size > best_size);
        
        if (is_better) {
            best_start = start;
            best_end = end;
            best_size = size;
            best_is_low = is_low;
            found = true;
        }
    }
    
    if (found) {
        out_start = best_start;
        out_end = best_end;
    }
    return found;
}

__PRIVILEGED_CODE int32_t bootstrap_allocator::init() {
    if (g_bootstrap_info.initialized) {
        return OK;
    }
    
    size_t required_pages = compute_required_pages();
    size_t required_size = required_pages * PAGE_SIZE;
    
    log::debug("bootstrap: need %lu pages (%lu KB) for page tables", 
               required_pages, required_size / 1024);
    
    phys_addr_t region_start, region_end;
    if (!find_suitable_region(required_pages, region_start, region_end)) {
        log::error("bootstrap: no suitable memory region found");
        return ERR_NO_MEMORY;
    }
    
    // Only reserve what we need from the region
    g_bootstrap_info.region_start = region_start;
    g_bootstrap_info.region_end = region_start + required_size;
    g_bootstrap_info.current = region_start;
    g_bootstrap_info.pages_allocated = 0;
    g_bootstrap_info.initialized = true;
    
    log::debug("bootstrap: using region 0x%lx-0x%lx (%lu pages available)",
               g_bootstrap_info.region_start, g_bootstrap_info.region_end,
               (g_bootstrap_info.region_end - g_bootstrap_info.region_start) / PAGE_SIZE);
    
    return OK;
}

__PRIVILEGED_CODE phys_addr_t bootstrap_allocator::alloc_page() {
    if (!g_bootstrap_info.initialized) {
        return 0;
    }
    if (g_bootstrap_info.current + PAGE_SIZE > g_bootstrap_info.region_end) {
        return 0; // Exhausted
    }
    
    phys_addr_t page = g_bootstrap_info.current;
    g_bootstrap_info.current += PAGE_SIZE;
    g_bootstrap_info.pages_allocated++;
    
    void* virt = reinterpret_cast<void*>(page + g_boot_info.hhdm_offset);
    memory::memset(virt, 0, PAGE_SIZE);
    return page;
}

__PRIVILEGED_CODE phys_addr_t bootstrap_allocator::get_region_start() {
    return g_bootstrap_info.region_start;
}

__PRIVILEGED_CODE phys_addr_t bootstrap_allocator::get_used_end() {
    return g_bootstrap_info.current;
}

__PRIVILEGED_CODE bool bootstrap_allocator::is_active() {
    return g_bootstrap_info.initialized && 
           (g_bootstrap_info.current + PAGE_SIZE <= g_bootstrap_info.region_end);
}

__PRIVILEGED_CODE size_t bootstrap_allocator::get_pages_allocated() {
    return g_bootstrap_info.pages_allocated;
}

__PRIVILEGED_CODE size_t bootstrap_allocator::get_pages_remaining() {
    if (!g_bootstrap_info.initialized) return 0;
    return (g_bootstrap_info.region_end - g_bootstrap_info.current) / PAGE_SIZE;
}

// Global PMM state
__PRIVILEGED_DATA pmm_state g_pmm = {};

// Debug assertion macro
#ifdef DEBUG
#define PMM_ASSERT(cond, msg) \
    do { if (!(cond)) log::fatal("PMM: " msg); } while(0)
#else
#define PMM_ASSERT(cond, msg) ((void)0)
#endif

template<typename T>
constexpr T min(T a, T b) { return (a < b) ? a : b; }

template<typename T>
constexpr T max(T a, T b) { return (a > b) ? a : b; }

__PRIVILEGED_CODE void freelist_add(zone& z, pfn_t pfn, uint8_t order) {
    free_area& fa = z.free_areas[order];
    page_frame_descriptor& pf = g_pmm.page_array[pfn];

    pf.list_prev = INVALID_PFN;
    pf.list_next = fa.first;
    pf.order = order;

    if (fa.first != INVALID_PFN) {
        g_pmm.page_array[fa.first].list_prev = pfn;
    } else {
        fa.last = pfn; // First element, also becomes last
    }
    fa.first = pfn;
    fa.count++;
}

__PRIVILEGED_CODE void freelist_remove(zone& z, pfn_t pfn, uint8_t order) {
    free_area& fa = z.free_areas[order];
    page_frame_descriptor& pf = g_pmm.page_array[pfn];

    if (pf.list_prev != INVALID_PFN) {
        g_pmm.page_array[pf.list_prev].list_next = pf.list_next;
    } else {
        fa.first = pf.list_next; // Was head
    }

    if (pf.list_next != INVALID_PFN) {
        g_pmm.page_array[pf.list_next].list_prev = pf.list_prev;
    } else {
        fa.last = pf.list_prev; // Was tail
    }

    pf.list_prev = INVALID_PFN;
    pf.list_next = INVALID_PFN;
    fa.count--;
}

__PRIVILEGED_CODE phys_addr_t zone_alloc(zone& z, uint8_t order) {
    // Find smallest order with free blocks >= requested
    for (uint8_t o = order; o <= MAX_ORDER; o++) {
        if (!z.free_areas[o].empty()) {
            // Remove block from freelist
            pfn_t pfn = z.free_areas[o].first;
            freelist_remove(z, pfn, o);

            // Split down to requested order
            while (o > order) {
                o--;
                // Add upper buddy to freelist
                pfn_t upper_buddy = pfn + (static_cast<pfn_t>(1) << o);
                g_pmm.page_array[upper_buddy].flags = PAGE_FLAG_NONE;
                g_pmm.page_array[upper_buddy].order = o;
                freelist_add(z, upper_buddy, o);
            }

            // Mark as allocated
            g_pmm.page_array[pfn].flags = PAGE_FLAG_ALLOCATED;
            g_pmm.page_array[pfn].order = order;
            z.free_pages -= order_to_pages(order);

            return pfn_to_phys(pfn);
        }
    }

    return 0; // No memory available
}

__PRIVILEGED_CODE int32_t zone_free(zone& z, pfn_t pfn, uint8_t order) {
    page_frame_descriptor& pf = g_pmm.page_array[pfn];

    // Validate not double-free
    if (pf.flags != PAGE_FLAG_ALLOCATED) {
        return ERR_DOUBLE_FREE;
    }

#ifdef DEBUG
    // Verify order matches what was stored during allocation
    if (pf.order != order) {
        log::error("PMM: order mismatch on free: stored=%u, passed=%u, pfn=0x%x",
                   pf.order, order, pfn);
        return ERR_ORDER_MISMATCH;
    }
#endif

    // Mark as free
    pf.flags = PAGE_FLAG_NONE;
    z.free_pages += order_to_pages(order);

    // Coalesce with buddy
    while (order < MAX_ORDER) {
        pfn_t buddy = buddy_pfn(pfn, order);

        // Check bounds
        if (buddy >= z.end_pfn || buddy < z.start_pfn) break;

        page_frame_descriptor& buddy_pf = g_pmm.page_array[buddy];

        // Check if buddy is free and at the same order
        if (!buddy_pf.is_free()) break;
        if (buddy_pf.order != order) break;

        // Remove buddy from its freelist
        freelist_remove(z, buddy, order);

        // Merge: use lower PFN as the combined block
        pfn = min(pfn, buddy);
        order++;
    }

    // Add merged block to freelist
    g_pmm.page_array[pfn].order = order;
    freelist_add(z, pfn, order);

    return OK;
}

// Find a suitable region for the page_frame_descriptor array
// Prefers regions above 4GB to preserve DMA32 zone
__PRIVILEGED_CODE static bool find_page_array_region(size_t required_size, phys_addr_t& out_phys) {
    phys_addr_t best_addr = 0;
    bool found = false;

    // First pass: look for regions above 4GB
    for (uint64_t i = 0; i < g_boot_info.memmap_entry_count; i++) {
        auto* e = g_boot_info.memmap_entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        phys_addr_t region_start = page_align_up(e->base);
        phys_addr_t region_end = page_align_down(e->base + e->length);

        if (region_end <= region_start) continue;
        if (region_end - region_start < required_size) continue;

        // Prefer above 4GB
        if (region_start >= ZONE_DMA32_LIMIT) {
            out_phys = region_start;
            return true;
        }

        // If region spans the 4GB boundary, use the part above 4GB
        if (region_start < ZONE_DMA32_LIMIT && region_end > ZONE_DMA32_LIMIT) {
            phys_addr_t high_start = ZONE_DMA32_LIMIT;
            if (region_end - high_start >= required_size) {
                out_phys = high_start;
                return true;
            }
        }

        // Track best DMA32 region as fallback
        if (!found || region_start > best_addr) {
            best_addr = region_start;
            found = true;
        }
    }

    if (found) {
        out_phys = best_addr;
        return true;
    }

    return false;
}

// Initialize zone structures
__PRIVILEGED_CODE static void init_zones() {
    // DMA32 zone: 0 - 4GB
    zone& dma32 = g_pmm.zones[static_cast<size_t>(zone_id::DMA32)];
    dma32.name = "DMA32";
    dma32.start_pfn = 0;
    dma32.end_pfn = phys_to_pfn(min(pfn_to_phys(g_pmm.max_pfn), ZONE_DMA32_LIMIT));
    dma32.total_pages = 0;
    dma32.free_pages = 0;
    for (uint8_t o = 0; o <= MAX_ORDER; o++) {
        dma32.free_areas[o].first = INVALID_PFN;
        dma32.free_areas[o].last = INVALID_PFN;
        dma32.free_areas[o].count = 0;
    }

    // Normal zone: 4GB+
    zone& normal = g_pmm.zones[static_cast<size_t>(zone_id::NORMAL)];
    normal.name = "Normal";
    if (g_pmm.max_pfn > dma32.end_pfn) {
        normal.start_pfn = dma32.end_pfn;
        normal.end_pfn = g_pmm.max_pfn;
    } else {
        // No memory above 4GB
        normal.start_pfn = 0;
        normal.end_pfn = 0;
    }
    normal.total_pages = 0;
    normal.free_pages = 0;
    for (uint8_t o = 0; o <= MAX_ORDER; o++) {
        normal.free_areas[o].first = INVALID_PFN;
        normal.free_areas[o].last = INVALID_PFN;
        normal.free_areas[o].count = 0;
    }
}

// Mark a range of pages with the specified flags
__PRIVILEGED_CODE static void mark_pages(pfn_t start_pfn, pfn_t end_pfn, uint8_t flags) {
    for (pfn_t pfn = start_pfn; pfn < end_pfn && pfn < g_pmm.max_pfn; pfn++) {
        g_pmm.page_array[pfn].flags = flags;
        g_pmm.page_array[pfn].zone = static_cast<uint8_t>(get_zone_for_pfn(pfn));
        g_pmm.page_array[pfn].order = 0;
        g_pmm.page_array[pfn].list_next = INVALID_PFN;
        g_pmm.page_array[pfn].list_prev = INVALID_PFN;
        g_pmm.page_array[pfn].refcount = 0;
    }
}

// Build freelists using max-order-first decomposition.
// For each contiguous run of free pages, directly computes the largest
// naturally-aligned power-of-2 block at each position. No coalescing
// pass needed — blocks go directly to their final order.
__PRIVILEGED_CODE static void build_freelists() {
    for (size_t zi = 0; zi < static_cast<size_t>(zone_id::COUNT); zi++) {
        zone& z = g_pmm.zones[zi];
        if (z.end_pfn <= z.start_pfn) continue;

        pfn_t pfn = z.start_pfn;
        while (pfn < z.end_pfn) {
            page_frame_descriptor& pf = g_pmm.page_array[pfn];

            // Count non-free, non-reserved pages toward total
            if (!pf.is_free() && !pf.is_reserved()) {
                z.total_pages++;
            }

            // Skip non-free pages
            if (!pf.is_free()) {
                pfn++;
                continue;
            }

            // Found start of a contiguous free run
            pfn_t run_start = pfn;
            while (pfn < z.end_pfn && g_pmm.page_array[pfn].is_free()) {
                pfn++;
            }
            pfn_t run_end = pfn;
            pfn_t run_pages = run_end - run_start;

            z.total_pages += run_pages;
            z.free_pages += run_pages;

            // Max-order-first decomposition of [run_start, run_end)
            pfn_t cur = run_start;
            while (cur < run_end) {
                // Find largest order where cur is naturally aligned and block fits
                uint8_t order = 0;
                while (order < MAX_ORDER) {
                    uint8_t next_order = static_cast<uint8_t>(order + 1);
                    pfn_t next_size = static_cast<pfn_t>(1) << next_order;
                    if (next_size > run_end - cur) break; // block too large
                    if ((cur & (next_size - 1)) != 0) break; // not aligned
                    order = next_order;
                }

                freelist_add(z, cur, order);
                cur += static_cast<pfn_t>(1) << order;
            }
        }
    }
}

// Check if a memory type should be tracked in our page array
__PRIVILEGED_CODE static bool is_tracked_memory_type(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        case LIMINE_MEMMAP_KERNEL_AND_MODULES:
        case LIMINE_MEMMAP_FRAMEBUFFER:
            return true;
        default:
            // RESERVED, ACPI_NVS, BAD_MEMORY - don't track distant MMIO regions
            return false;
    }
}

__PRIVILEGED_CODE int32_t init() {
    if (g_pmm.initialized) {
        return OK;
    }

    // Initialize bootstrap allocator first
    int32_t bs_result = bootstrap_allocator::init();
    if (bs_result != OK) {
        log::error("PMM: bootstrap_allocator::init failed");
        return bs_result;
    }

    // Find highest tracked address (excludes distant MMIO regions)
    phys_addr_t highest_addr = 0;
    for (uint64_t i = 0; i < g_boot_info.memmap_entry_count; i++) {
        auto* e = g_boot_info.memmap_entries[i];
        if (!is_tracked_memory_type(e->type)) continue;

        phys_addr_t end = e->base + e->length;
        if (end > highest_addr) {
            highest_addr = end;
        }
    }

    g_pmm.max_pfn = phys_to_pfn(page_align_up(highest_addr));
    g_pmm.page_array_size = static_cast<size_t>(g_pmm.max_pfn) * sizeof(page_frame_descriptor);

    log::debug("PMM: highest_addr=0x%lx, max_pfn=%u, page_array_size=0x%lx",
               highest_addr, g_pmm.max_pfn, g_pmm.page_array_size);

    // Find region for page array (prefer above 4GB)
    if (!find_page_array_region(g_pmm.page_array_size, g_pmm.page_array_phys)) {
        log::error("PMM: cannot find region for page array (0x%lx bytes)",
                   g_pmm.page_array_size);
        return ERR_NO_MEMORY;
    }

    g_pmm.page_array = reinterpret_cast<page_frame_descriptor*>(
        g_pmm.page_array_phys + g_boot_info.hhdm_offset
    );

    log::debug("PMM: page_array at phys=0x%lx, virt=0x%lx",
               g_pmm.page_array_phys,
               reinterpret_cast<uint64_t>(g_pmm.page_array));

    memory::memset(g_pmm.page_array, 0, g_pmm.page_array_size);
    init_zones();

    // Mark all pages reserved, then mark usable regions as free
    mark_pages(0, g_pmm.max_pfn, PAGE_FLAG_RESERVED);
    for (uint64_t i = 0; i < g_boot_info.memmap_entry_count; i++) {
        auto* e = g_boot_info.memmap_entries[i];

        pfn_t start_pfn = phys_to_pfn(page_align_up(e->base));
        pfn_t end_pfn = phys_to_pfn(page_align_down(e->base + e->length));

        if (end_pfn <= start_pfn) continue;

        if (e->type == LIMINE_MEMMAP_USABLE) {
            mark_pages(start_pfn, end_pfn, PAGE_FLAG_NONE); // Free
        }
    }

    // Mark page array region itself as reserved
    pfn_t array_start_pfn = phys_to_pfn(g_pmm.page_array_phys);
    pfn_t array_end_pfn = phys_to_pfn(page_align_up(g_pmm.page_array_phys + g_pmm.page_array_size));
    mark_pages(array_start_pfn, array_end_pfn, PAGE_FLAG_RESERVED);

    // Reserve bootstrap allocator's entire region (it will be used by paging::init())
    // We reserve the full region up-front since paging::init() hasn't run yet
    if (bootstrap_allocator::is_active()) {
        pfn_t bs_start_pfn = phys_to_pfn(bootstrap_allocator::get_region_start());
        // Reserve full region end, not just used_end (which is still 0 at this point)
        phys_addr_t region_end = bootstrap_allocator::get_region_start() + 
            (bootstrap_allocator::get_pages_remaining() + bootstrap_allocator::get_pages_allocated()) * PAGE_SIZE;
        pfn_t bs_end_pfn = phys_to_pfn(region_end);
        mark_pages(bs_start_pfn, bs_end_pfn, PAGE_FLAG_RESERVED);
        log::debug("PMM: reserved %lu bootstrap pages at 0x%lx",
                   bs_end_pfn - bs_start_pfn, bootstrap_allocator::get_region_start());
    }

    // Build freelists and coalesce
    build_freelists();

    g_pmm.initialized = true;
    dump_stats();

    // Initialize paging now that PMM is ready
    if (paging::init() != paging::OK) {
        log::fatal("paging::init failed");
    }

    paging::dump_mappings();
    return OK;
}

__PRIVILEGED_CODE phys_addr_t alloc_pages(uint8_t order, zone_mask_t zones) {
    if (!g_pmm.initialized) return 0;
    if (order > MAX_ORDER) return 0;

    // TODO: Check per-CPU cache first for order == 0
    // For now, go directly to zone allocator

    // Try ZONE_NORMAL first to preserve DMA32 memory
    if (zones & ZONE_NORMAL) {
        zone& z = g_pmm.zones[static_cast<size_t>(zone_id::NORMAL)];
        if (z.end_pfn > z.start_pfn) {
            phys_addr_t addr = zone_alloc(z, order);
            if (addr != 0) return addr;
        }
    }

    // Fall back to ZONE_DMA32
    if (zones & ZONE_DMA32) {
        zone& z = g_pmm.zones[static_cast<size_t>(zone_id::DMA32)];
        return zone_alloc(z, order);
    }

    return 0;
}

__PRIVILEGED_CODE int32_t free_pages(phys_addr_t addr, uint8_t order) {
    if (!g_pmm.initialized) return ERR_NOT_INITIALIZED;
    if (order > MAX_ORDER) return ERR_INVALID_ORDER;

    // Check alignment
    phys_addr_t required_align = order_to_bytes(order);
    if ((addr & (required_align - 1)) != 0) {
        return ERR_INVALID_ADDR;
    }

    pfn_t pfn = phys_to_pfn(addr);
    if (pfn >= g_pmm.max_pfn) {
        return ERR_INVALID_ADDR;
    }

    // Determine zone and free
    zone_id zid = get_zone_for_pfn(pfn);
    zone& z = g_pmm.zones[static_cast<size_t>(zid)];

    // TODO: Return to per-CPU cache for order == 0
    return zone_free(z, pfn, order);
}

__PRIVILEGED_CODE phys_addr_t alloc_page(zone_mask_t zones) {
    return alloc_pages(0, zones);
}

__PRIVILEGED_CODE int32_t free_page(phys_addr_t addr) {
    return free_pages(addr, 0);
}

__PRIVILEGED_CODE page_frame_descriptor* get_page_frame(phys_addr_t addr) {
    if (!g_pmm.initialized) return nullptr;

    pfn_t pfn = phys_to_pfn(addr);
    if (pfn >= g_pmm.max_pfn) return nullptr;

    return &g_pmm.page_array[pfn];
}

__PRIVILEGED_CODE uint64_t free_page_count(zone_mask_t zones) {
    if (!g_pmm.initialized) return 0;

    uint64_t total = 0;
    for (size_t zi = 0; zi < static_cast<size_t>(zone_id::COUNT); zi++) {
        if (zones & (1 << zi)) {
            total += g_pmm.zones[zi].free_pages;
        }
    }
    return total;
}

__PRIVILEGED_CODE uint64_t free_block_count(uint8_t order, zone_mask_t zones) {
    if (!g_pmm.initialized) return 0;
    if (order > MAX_ORDER) return 0;

    uint64_t total = 0;
    for (size_t zi = 0; zi < static_cast<size_t>(zone_id::COUNT); zi++) {
        if (zones & (1 << zi)) {
            total += g_pmm.zones[zi].free_areas[order].count;
        }
    }
    return total;
}

} // namespace pmm
