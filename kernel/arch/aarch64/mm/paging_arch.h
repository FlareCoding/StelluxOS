#ifndef STELLUX_ARCH_AARCH64_PAGING_ARCH_H
#define STELLUX_ARCH_AARCH64_PAGING_ARCH_H

#include "types.h"

namespace paging {

// Table descriptor (L0, L1, L2 pointing to next level table)
struct table_desc_t {
    union {
        struct {
            uint64_t valid             : 1;   // [0] Must be 1 for valid entry
            uint64_t type              : 1;   // [1] Must be 1 for table descriptor
            uint64_t ignored1          : 10;  // [2-11] Ignored
            uint64_t next_table_addr   : 36;  // [12-47] Next level table phys >> 12
            uint64_t reserved          : 4;   // [48-51] Reserved
            uint64_t ignored2          : 7;   // [52-58] Ignored
            uint64_t pxn_table         : 1;   // [59] PXN limit for subsequent levels
            uint64_t uxn_table         : 1;   // [60] UXN limit for subsequent levels
            uint64_t ap_table          : 2;   // [61-62] AP limit for subsequent levels
            uint64_t ns_table          : 1;   // [63] Security bit for subsequent levels
        };
        uint64_t value;
    };
};
static_assert(sizeof(table_desc_t) == 8);

// Block descriptor (L1 = 1GB block, L2 = 2MB block)
struct block_desc_t {
    union {
        struct {
            uint64_t valid             : 1;   // [0] Must be 1 for valid entry
            uint64_t type              : 1;   // [1] Must be 0 for block descriptor
            uint64_t attr_idx          : 3;   // [2-4] MAIR index for memory attributes
            uint64_t ns                : 1;   // [5] Non-secure bit
            uint64_t ap                : 2;   // [6-7] Access permission
            uint64_t sh                : 2;   // [8-9] Shareability
            uint64_t af                : 1;   // [10] Access flag (must be 1 or fault)
            uint64_t ng                : 1;   // [11] Not global (0 = global)
            uint64_t reserved1         : 9;   // [12-20] Reserved (L2) / part of OA (L1)
            uint64_t output_addr       : 27;  // [21-47] Output address >> 21 (2MB)
            uint64_t reserved2         : 4;   // [48-51] Reserved
            uint64_t contiguous        : 1;   // [52] Contiguous hint
            uint64_t pxn               : 1;   // [53] Privileged execute never
            uint64_t uxn               : 1;   // [54] User execute never
            uint64_t software          : 4;   // [55-58] Available for OS use
            uint64_t ignored           : 5;   // [59-63] Ignored
        };
        uint64_t value;
    };
};
static_assert(sizeof(block_desc_t) == 8);

// Page descriptor (L3 = 4KB page, final level)
struct page_desc_t {
    union {
        struct {
            uint64_t valid             : 1;   // [0] Must be 1 for valid entry
            uint64_t type              : 1;   // [1] Must be 1 for page descriptor
            uint64_t attr_idx          : 3;   // [2-4] MAIR index for memory attributes
            uint64_t ns                : 1;   // [5] Non-secure bit
            uint64_t ap                : 2;   // [6-7] Access permission
            uint64_t sh                : 2;   // [8-9] Shareability
            uint64_t af                : 1;   // [10] Access flag (must be 1 or fault)
            uint64_t ng                : 1;   // [11] Not global (0 = global)
            uint64_t output_addr       : 36;  // [12-47] Output address >> 12
            uint64_t reserved          : 4;   // [48-51] Reserved
            uint64_t contiguous        : 1;   // [52] Contiguous hint
            uint64_t pxn               : 1;   // [53] Privileged execute never
            uint64_t uxn               : 1;   // [54] User execute never
            uint64_t software          : 4;   // [55-58] Available for OS use
            uint64_t ignored           : 5;   // [59-63] Ignored
        };
        uint64_t value;
    };
};
static_assert(sizeof(page_desc_t) == 8);

// Access Permission (AP) field values
namespace ap {
    constexpr uint64_t EL1_RW_EL0_NONE = 0b00;  // Kernel R/W, User no access
    constexpr uint64_t EL1_RW_EL0_RW   = 0b01;  // Kernel R/W, User R/W
    constexpr uint64_t EL1_RO_EL0_NONE = 0b10;  // Kernel R/O, User no access
    constexpr uint64_t EL1_RO_EL0_RO   = 0b11;  // Kernel R/O, User R/O
}

// Shareability (SH) field values
namespace sh {
    constexpr uint64_t NON_SHAREABLE   = 0b00;
    constexpr uint64_t OUTER_SHAREABLE = 0b10;
    constexpr uint64_t INNER_SHAREABLE = 0b11;
}

// MAIR indices (must match MAIR_EL1 configuration)
// NOTE: Index 0 is reserved for Limine's use (0xFF = Normal Write-Back)
//       We use index 1 for our normal memory to avoid conflicts
namespace mair_idx {
    constexpr uint64_t LIMINE_NORMAL   = 0;  // Limine uses this for normal cacheable memory (0xFF)
    constexpr uint64_t NORMAL_WB       = 1;  // Normal, write-back cacheable (our normal memory)
    constexpr uint64_t NORMAL_NC       = 2;  // Normal, non-cacheable (write-combine)
    constexpr uint64_t DEVICE_nGnRnE   = 3;  // Device memory, strongly ordered (moved from index 0)
}

// MAIR attribute encodings
namespace mair_attr {
    constexpr uint64_t LIMINE_NORMAL   = 0xFF;  // Limine's normal memory (Write-Back, RW-Allocate)
    constexpr uint64_t NORMAL_WB       = 0xFF;  // Normal, Write-Back, Read/Write Allocate
    constexpr uint64_t NORMAL_NC       = 0x44;  // Normal, Non-Cacheable
    constexpr uint64_t DEVICE_nGnRnE   = 0x00;  // Device-nGnRnE
}

// Translation table structure (512 entries, 4KB aligned)
struct translation_table_t {
    union {
        table_desc_t as_table[512];
        block_desc_t as_block[512];
        page_desc_t  as_page[512];
        uint64_t     raw[512];
    };
} __attribute__((aligned(4096)));

// Virtual address breakdown helper (4KB granule, 48-bit VA)
struct virt_addr_parts_t {
    uint16_t l0_idx;    // Bits 47:39 (9 bits)
    uint16_t l1_idx;    // Bits 38:30 (9 bits)
    uint16_t l2_idx;    // Bits 29:21 (9 bits)
    uint16_t l3_idx;    // Bits 20:12 (9 bits)
    uint16_t offset;    // Bits 11:0  (12 bits)
};

constexpr virt_addr_parts_t split_virt_addr(uint64_t addr) {
    return {
        .l0_idx = static_cast<uint16_t>((addr >> 39) & 0x1FF),
        .l1_idx = static_cast<uint16_t>((addr >> 30) & 0x1FF),
        .l2_idx = static_cast<uint16_t>((addr >> 21) & 0x1FF),
        .l3_idx = static_cast<uint16_t>((addr >> 12) & 0x1FF),
        .offset = static_cast<uint16_t>(addr & 0xFFF),
    };
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void write_ttbr1_el1(uint64_t val) {
    asm volatile("msr ttbr1_el1, %0" :: "r"(val));
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline uint64_t read_ttbr1_el1() {
    uint64_t val;
    asm volatile("mrs %0, ttbr1_el1" : "=r"(val));
    return val;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void write_ttbr0_el1(uint64_t val) {
    asm volatile(
        "msr ttbr0_el1, %0\n\t"
        "isb\n\t"
        "tlbi vmalle1is\n\t"
        "dsb ish\n\t"
        "isb"
        :: "r"(val) : "memory"
    );
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline uint64_t read_ttbr0_el1() {
    uint64_t val;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(val));
    return val;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void write_mair_el1(uint64_t val) {
    asm volatile("msr mair_el1, %0" :: "r"(val));
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline uint64_t read_mair_el1() {
    uint64_t val;
    asm volatile("mrs %0, mair_el1" : "=r"(val));
    return val;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void write_tcr_el1(uint64_t val) {
    asm volatile("msr tcr_el1, %0" :: "r"(val));
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline uint64_t read_tcr_el1() {
    uint64_t val;
    asm volatile("mrs %0, tcr_el1" : "=r"(val));
    return val;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline uint64_t read_sctlr_el1() {
    uint64_t val;
    asm volatile("mrs %0, sctlr_el1" : "=r"(val));
    return val;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void write_sctlr_el1(uint64_t val) {
    asm volatile("msr sctlr_el1, %0" :: "r"(val) : "memory");
    asm volatile("isb" ::: "memory");
}

// SCTLR_EL1 bit definitions
namespace sctlr {
    constexpr uint64_t M    = 1ULL << 0;   // MMU enable
    constexpr uint64_t C    = 1ULL << 2;   // D-cache enable
    constexpr uint64_t I    = 1ULL << 12;  // I-cache enable
    constexpr uint64_t WXN  = 1ULL << 19;  // Write permission implies XN (Execute Never)
    constexpr uint64_t SPAN = 1ULL << 23;  // Set Privileged Access Never
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline uint64_t read_vbar_el1() {
    uint64_t val;
    asm volatile("mrs %0, vbar_el1" : "=r"(val));
    return val;
}

/**
 * TLB invalidation with proper barrier sequence
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void tlbi_vmalle1is() {
    asm volatile(
        "dsb ishst\n"
        "tlbi vmalle1is\n"
        "dsb ish\n"
        "isb"
        ::: "memory"
    );
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void tlbi_vae1is(uint64_t addr) {
    // Address should be VA >> 12
    uint64_t va_shifted = addr >> 12;
    asm volatile(
        "dsb ishst\n"
        "tlbi vae1is, %0\n"
        "dsb ish\n"
        "isb"
        :: "r"(va_shifted) : "memory"
    );
}

/**
 * Local (non-shareable) TLB invalidation
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void tlbi_vmalle1() {
    asm volatile(
        "dsb nshst\n"
        "tlbi vmalle1\n"
        "dsb nsh\n"
        "isb"
        ::: "memory"
    );
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void tlbi_vae1(uint64_t addr) {
    uint64_t va_shifted = addr >> 12;
    asm volatile(
        "dsb nshst\n"
        "tlbi vae1, %0\n"
        "dsb nsh\n"
        "isb"
        :: "r"(va_shifted) : "memory"
    );
}

} // namespace paging

#endif // STELLUX_ARCH_AARCH64_PAGING_ARCH_H
