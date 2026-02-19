#ifndef STELLUX_MM_PMM_TYPES_H
#define STELLUX_MM_PMM_TYPES_H

#include "types.h"

namespace pmm {

// Physical address type (64-bit, full physical address space)
using phys_addr_t = uint64_t;
using pfn_t = uint32_t;

constexpr pfn_t INVALID_PFN = 0xFFFFFFFF;

constexpr uint64_t PAGE_SIZE  = 4096;
constexpr uint64_t PAGE_SHIFT = 12;
constexpr uint64_t PAGE_MASK  = ~(PAGE_SIZE - 1);

constexpr uint8_t MAX_ORDER = 18; // 2^18 pages = 1GB max block

constexpr uint8_t ORDER_4KB  = 0;
constexpr uint8_t ORDER_8KB  = 1;
constexpr uint8_t ORDER_16KB = 2;
constexpr uint8_t ORDER_64KB = 4;
constexpr uint8_t ORDER_2MB  = 9;
constexpr uint8_t ORDER_4MB  = 10;
constexpr uint8_t ORDER_1GB  = 18;

// Zone identifiers
enum class zone_id : uint8_t {
    DMA32  = 0, // 0 - 4GB
    NORMAL = 1, // 4GB+
    COUNT  = 2
};

constexpr uint8_t ZONE_DMA32  = (1 << static_cast<uint8_t>(zone_id::DMA32));
constexpr uint8_t ZONE_NORMAL = (1 << static_cast<uint8_t>(zone_id::NORMAL));
constexpr uint8_t ZONE_ANY    = ZONE_DMA32 | ZONE_NORMAL;
using zone_mask_t = uint8_t;

constexpr uint8_t PAGE_FLAG_NONE      = 0;
constexpr uint8_t PAGE_FLAG_ALLOCATED = (1 << 0);
constexpr uint8_t PAGE_FLAG_RESERVED  = (1 << 1);
constexpr uint8_t PAGE_FLAG_HUGE_HEAD = (1 << 2);
constexpr uint8_t PAGE_FLAG_HUGE_TAIL = (1 << 3);
constexpr uint8_t PAGE_FLAG_SLAB      = (1 << 4);

constexpr int32_t OK                  = 0;
constexpr int32_t ERR_NO_MEMORY       = -1;
constexpr int32_t ERR_INVALID_ORDER   = -2;
constexpr int32_t ERR_INVALID_ADDR    = -3;
constexpr int32_t ERR_DOUBLE_FREE     = -4;
constexpr int32_t ERR_NOT_INITIALIZED = -5;
constexpr int32_t ERR_ORDER_MISMATCH  = -6;

// Page frame descriptor - metadata for a single physical page
// One of these exists for every physical page in the system (16 bytes each)
struct page_frame_descriptor {
    uint8_t  flags;         // Flags indicating page state (free, allocated, reserved, etc.)
    uint8_t  zone;          // Zone ID (DMA32 or NORMAL)
    uint8_t  order;         // Order of the free block (0=1 page, 1=2 pages, ..., 18=262144 pages)
    uint8_t  _reserved;     // Reserved for future use
    pfn_t    list_next;     // Next page in free list
    pfn_t    list_prev;     // Previous page in free list
    uint32_t refcount;      // Reference count for page

    bool is_free() const { return flags == PAGE_FLAG_NONE; }
    bool is_allocated() const { return (flags & PAGE_FLAG_ALLOCATED) != 0; }
    bool is_reserved() const { return (flags & PAGE_FLAG_RESERVED) != 0; }
};
static_assert(sizeof(page_frame_descriptor) == 16, "page_frame_descriptor size must be 16 bytes");

// Convert physical address to page frame number
constexpr pfn_t phys_to_pfn(phys_addr_t addr) {
    return static_cast<pfn_t>(addr >> PAGE_SHIFT);
}

// Convert page frame number to physical address
constexpr phys_addr_t pfn_to_phys(pfn_t pfn) {
    return static_cast<phys_addr_t>(pfn) << PAGE_SHIFT;
}

// Align down to nearest page boundary
constexpr phys_addr_t page_align_down(phys_addr_t addr) {
    return addr & PAGE_MASK;
}

// Align up to nearest page boundary
constexpr phys_addr_t page_align_up(phys_addr_t addr) {
    return (addr + PAGE_SIZE - 1) & PAGE_MASK;
}

// Convert number of pages to order (0=1 page, 1=2 pages, ..., 10=1024 pages)
constexpr uint8_t pages_to_order(size_t pages) {
    if (pages <= 1) return 0;
    uint8_t order = 0;
    size_t block_pages = 1;
    while (block_pages < pages && order < MAX_ORDER) {
        order++;
        block_pages <<= 1;
    }
    return order;
}

// Calculate number of pages for a given order
constexpr size_t order_to_pages(uint8_t order) {
    return static_cast<size_t>(1) << order;
}

// Calculate number of bytes for a given order
constexpr size_t order_to_bytes(uint8_t order) {
    return order_to_pages(order) * PAGE_SIZE;
}

} // namespace pmm

#endif // STELLUX_MM_PMM_TYPES_H
