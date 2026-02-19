#ifndef STELLUX_ARCH_X86_64_PAGING_ARCH_H
#define STELLUX_ARCH_X86_64_PAGING_ARCH_H

#include "types.h"

namespace paging {

// Page Map Level 4 Entry (also used for PDPT, PD table entries)
struct pml4e_t {
    union {
        struct {
            uint64_t present           : 1;   // [0] Must be 1 for valid entry
            uint64_t read_write        : 1;   // [1] 0=read-only, 1=read-write
            uint64_t user_supervisor   : 1;   // [2] 0=supervisor, 1=user accessible
            uint64_t page_write_through: 1;   // [3] PWT - write-through caching
            uint64_t page_cache_disable: 1;   // [4] PCD - disable caching
            uint64_t accessed          : 1;   // [5] Set by CPU on access
            uint64_t ignored1          : 1;   // [6] Ignored
            uint64_t page_size         : 1;   // [7] Must be 0 for table pointer
            uint64_t ignored2          : 4;   // [8-11] Ignored
            uint64_t phys_addr         : 40;  // [12-51] Physical address >> 12
            uint64_t ignored3          : 11;  // [52-62] Ignored
            uint64_t execute_disable   : 1;   // [63] NX bit - no execute
        };
        uint64_t value;
    };
};
static_assert(sizeof(pml4e_t) == 8);

// PDPT Entry (table pointer form, same layout as pml4e_t)
using pdpte_t = pml4e_t;

// Page Directory Entry (table pointer form, same layout as pml4e_t)
using pde_t = pml4e_t;

// Page Table Entry (4KB page, final level)
struct pte_t {
    union {
        struct {
            uint64_t present           : 1;   // [0] Must be 1 for valid entry
            uint64_t read_write        : 1;   // [1] 0=read-only, 1=read-write
            uint64_t user_supervisor   : 1;   // [2] 0=supervisor, 1=user accessible
            uint64_t page_write_through: 1;   // [3] PWT
            uint64_t page_cache_disable: 1;   // [4] PCD
            uint64_t accessed          : 1;   // [5] Set by CPU on access
            uint64_t dirty             : 1;   // [6] Set by CPU on write
            uint64_t pat               : 1;   // [7] PAT bit for memory type
            uint64_t global            : 1;   // [8] Global page (survives CR3 switch)
            uint64_t ignored1          : 3;   // [9-11] Ignored
            uint64_t phys_addr         : 40;  // [12-51] Physical address >> 12
            uint64_t ignored2          : 7;   // [52-58] Ignored
            uint64_t protection_key    : 4;   // [59-62] Protection key (if CR4.PKE)
            uint64_t execute_disable   : 1;   // [63] NX bit
        };
        uint64_t value;
    };
};
static_assert(sizeof(pte_t) == 8);

// Page Directory Entry for 2MB large page (PS=1)
struct pde_2mb_t {
    union {
        struct {
            uint64_t present           : 1;   // [0]
            uint64_t read_write        : 1;   // [1]
            uint64_t user_supervisor   : 1;   // [2]
            uint64_t page_write_through: 1;   // [3]
            uint64_t page_cache_disable: 1;   // [4]
            uint64_t accessed          : 1;   // [5]
            uint64_t dirty             : 1;   // [6]
            uint64_t page_size         : 1;   // [7] Must be 1 for 2MB page
            uint64_t global            : 1;   // [8]
            uint64_t ignored1          : 3;   // [9-11]
            uint64_t pat               : 1;   // [12] PAT bit for large pages
            uint64_t reserved          : 8;   // [13-20] Must be 0
            uint64_t phys_addr         : 31;  // [21-51] Physical address >> 21
            uint64_t ignored2          : 7;   // [52-58]
            uint64_t protection_key    : 4;   // [59-62]
            uint64_t execute_disable   : 1;   // [63]
        };
        uint64_t value;
    };
};
static_assert(sizeof(pde_2mb_t) == 8);

// PDPT Entry for 1GB huge page (PS=1)
struct pdpte_1gb_t {
    union {
        struct {
            uint64_t present           : 1;   // [0]
            uint64_t read_write        : 1;   // [1]
            uint64_t user_supervisor   : 1;   // [2]
            uint64_t page_write_through: 1;   // [3]
            uint64_t page_cache_disable: 1;   // [4]
            uint64_t accessed          : 1;   // [5]
            uint64_t dirty             : 1;   // [6]
            uint64_t page_size         : 1;   // [7] Must be 1 for 1GB page
            uint64_t global            : 1;   // [8]
            uint64_t ignored1          : 3;   // [9-11]
            uint64_t pat               : 1;   // [12] PAT bit
            uint64_t reserved          : 17;  // [13-29] Must be 0
            uint64_t phys_addr         : 22;  // [30-51] Physical address >> 30
            uint64_t ignored2          : 7;   // [52-58]
            uint64_t protection_key    : 4;   // [59-62]
            uint64_t execute_disable   : 1;   // [63]
        };
        uint64_t value;
    };
};
static_assert(sizeof(pdpte_1gb_t) == 8);

// Page table structures (512 entries each, 4KB aligned)
struct page_table_t {
    pte_t entries[512];
} __attribute__((aligned(4096)));

struct page_directory_t {
    pde_t entries[512];
} __attribute__((aligned(4096)));

struct pdpt_t {
    pdpte_t entries[512];
} __attribute__((aligned(4096)));

struct pml4_t {
    pml4e_t entries[512];
} __attribute__((aligned(4096)));

// Virtual address breakdown helper
struct virt_addr_parts_t {
    uint16_t pml4_idx;   // Bits 47:39 (9 bits)
    uint16_t pdpt_idx;   // Bits 38:30 (9 bits)
    uint16_t pd_idx;     // Bits 29:21 (9 bits)
    uint16_t pt_idx;     // Bits 20:12 (9 bits)
    uint16_t offset;     // Bits 11:0  (12 bits)
};

constexpr virt_addr_parts_t split_virt_addr(uint64_t addr) {
    return {
        .pml4_idx = static_cast<uint16_t>((addr >> 39) & 0x1FF),
        .pdpt_idx = static_cast<uint16_t>((addr >> 30) & 0x1FF),
        .pd_idx   = static_cast<uint16_t>((addr >> 21) & 0x1FF),
        .pt_idx   = static_cast<uint16_t>((addr >> 12) & 0x1FF),
        .offset   = static_cast<uint16_t>(addr & 0xFFF),
    };
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void write_cr3(uint64_t val) {
    asm volatile("mov %0, %%cr3" :: "r"(val) : "memory");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline uint64_t read_cr3() {
    uint64_t val;
    asm volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

/**
 * TLB invalidation for a specific virtual address.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void invlpg(uint64_t addr) {
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

} // namespace paging

#endif // STELLUX_ARCH_X86_64_PAGING_ARCH_H
