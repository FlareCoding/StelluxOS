#ifndef STELLUX_ARCH_X86_64_GDT_GDT_H
#define STELLUX_ARCH_X86_64_GDT_GDT_H

#include "common/types.h"

namespace x86 {

// Segment Descriptor (Intel SDM Vol. 3A, Section 3.4.5, Figure 3-8)
//
// Standard 8-byte segment descriptor for code/data segments.
// In 64-bit long mode, base and limit are ignored for CS/DS (flat model),
// but we define the full structure for correctness.
//
// Bit Layout:
//   Bytes 0-1: Limit[15:0]
//   Bytes 2-3: Base[15:0]
//   Byte 4:    Base[23:16]
//   Byte 5:    Access byte (Type[3:0], S, DPL[1:0], P)
//   Byte 6:    Limit[19:16] | Flags (AVL, L, D/B, G)
//   Byte 7:    Base[31:24]
struct __attribute__((packed)) segment_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_high_flags;
    uint8_t  base_high;
};

static_assert(sizeof(segment_descriptor) == 8, "segment_descriptor must be 8 bytes");

// Access byte bit definitions (Intel SDM Vol. 3A, Table 3-1)
namespace access {
    constexpr uint8_t ACCESSED    = 0x01;
    constexpr uint8_t RW          = 0x02;
    constexpr uint8_t DC          = 0x04;
    constexpr uint8_t EXEC        = 0x08;
    constexpr uint8_t CODE_DATA   = 0x10;
    constexpr uint8_t DPL_RING0   = 0x00;
    constexpr uint8_t DPL_RING3   = 0x60;
    constexpr uint8_t PRESENT     = 0x80;

    constexpr uint8_t KERNEL_CODE = PRESENT | CODE_DATA | EXEC | RW;
    constexpr uint8_t KERNEL_DATA = PRESENT | CODE_DATA | RW;
    constexpr uint8_t USER_CODE   = PRESENT | DPL_RING3 | CODE_DATA | EXEC | RW;
    constexpr uint8_t USER_DATA   = PRESENT | DPL_RING3 | CODE_DATA | RW;
}

// Flags nibble bit definitions (upper 4 bits of byte 6)
namespace flags {
    constexpr uint8_t AVL         = 0x10; // Bit 4: Available for system use
    constexpr uint8_t LONG_MODE   = 0x20; // Bit 5: L=1 for 64-bit code segment
    constexpr uint8_t SIZE_32     = 0x40; // Bit 6: D/B=1 for 32-bit default size
    constexpr uint8_t GRANULARITY = 0x80; // Bit 7: G=1 for 4KB granularity

    // Common combinations
    constexpr uint8_t CODE_64     = GRANULARITY | LONG_MODE; // 0xA0: 64-bit code
    constexpr uint8_t DATA_64     = GRANULARITY | SIZE_32;   // 0xC0: 64-bit data
}

// System Segment Descriptor (Intel SDM Vol. 3A, Section 7.2.3, Figure 7-4)
//
// 64-bit TSS and LDT descriptors are 16 bytes (two consecutive GDT slots).
// The upper 8 bytes contain the high 32 bits of the base address.
struct __attribute__((packed)) system_segment_descriptor {
    // First 8 bytes (same layout as segment_descriptor)
    uint16_t limit_low;      // Segment limit bits 15:0
    uint16_t base_low;       // Base address bits 15:0
    uint8_t  base_mid_low;   // Base address bits 23:16
    uint8_t  access;         // Access byte: P(1) DPL(2) 0(1) Type(4)
    uint8_t  limit_high_flags; // Limit[19:16] | G(1) 0(2) AVL(1)
    uint8_t  base_mid_high; // Base address bits 31:24
    // Second 8 bytes (64-bit extension)
    uint32_t base_high;      // Base address bits 63:32
    uint32_t reserved;       // Reserved, must be 0
};

static_assert(sizeof(system_segment_descriptor) == 16, "system_segment_descriptor must be 16 bytes");

// System segment types (Intel SDM Vol. 3A, Table 3-2)
namespace system_type {
    constexpr uint8_t TSS_AVAILABLE = 0x09; // 64-bit TSS (Available)
    constexpr uint8_t TSS_BUSY      = 0x0B; // 64-bit TSS (Busy)
    constexpr uint8_t LDT           = 0x02; // LDT
}

// 64-bit TSS Structure (Intel SDM Vol. 3A, Section 7.7, Figure 7-11)
//
// The Task State Segment in 64-bit mode. Used for:
// - RSP0-RSP2: Stack pointers for privilege level changes
// - IST1-IST7: Interrupt Stack Table entries for dedicated exception stacks
// - I/O Permission Bitmap (optional)
struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;   // I/O Permission Bitmap offset from TSS base
};

static_assert(sizeof(tss) == 104, "TSS must be 104 bytes");

struct __attribute__((packed)) gdt_ptr {
    uint16_t limit;
    uint64_t base;
};

static_assert(sizeof(gdt_ptr) == 10, "gdt_ptr must be 10 bytes");
static_assert(__builtin_offsetof(gdt_ptr, base) == 2, "gdt_ptr.base must be at offset 2");

union gdt_entry {
    segment_descriptor segment;
    uint64_t raw;
};

static_assert(sizeof(gdt_entry) == 8, "gdt_entry must be 8 bytes");

constexpr segment_descriptor make_null_descriptor() {
    return segment_descriptor{0, 0, 0, 0, 0, 0};
}

constexpr segment_descriptor make_segment_descriptor(uint8_t access_byte, uint8_t flags_nibble) {
    return segment_descriptor {
        .limit_low = 0xFFFF,
        .base_low = 0x0000,
        .base_mid = 0x00,
        .access = access_byte,
        .limit_high_flags = static_cast<uint8_t>((flags_nibble & 0xF0) | 0x0F),
        .base_high = 0x00
    };
}

/**
 * @brief Create a system segment descriptor (TSS/LDT).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline system_segment_descriptor make_system_descriptor(uint64_t base, uint32_t limit, uint8_t type) {
    return system_segment_descriptor{
        .limit_low = static_cast<uint16_t>(limit & 0xFFFF),
        .base_low = static_cast<uint16_t>(base & 0xFFFF),
        .base_mid_low = static_cast<uint8_t>((base >> 16) & 0xFF),
        .access = static_cast<uint8_t>(access::PRESENT | (type & 0x0F)),
        .limit_high_flags = static_cast<uint8_t>((limit >> 16) & 0x0F),
        .base_mid_high = static_cast<uint8_t>((base >> 24) & 0xFF),
        .base_high = static_cast<uint32_t>((base >> 32) & 0xFFFFFFFF),
        .reserved = 0
    };
}

namespace gdt {

constexpr int32_t OK = 0;
constexpr int32_t ERR_INIT = -1;

/**
 * @brief Initialize GDT entries and TSS with explicit stack pointers.
 * Use this for APs with VMM-allocated stacks.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init(uintptr_t rsp0, uintptr_t ist1, uintptr_t ist2, uintptr_t ist3);

/**
 * @brief Initialize GDT for BSP using static stacks.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_bsp();

/**
 * @brief Get the BSP kernel stack top (same value as TSS.RSP0).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uintptr_t get_bsp_kernel_stack_top();

/**
 * @brief Load GDT and reload segment registers.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void load();

/**
 * @brief Update TSS.RSP0 for the current CPU (context switch).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_rsp0(uintptr_t rsp0);

} // namespace gdt
} // namespace x86

#endif // STELLUX_ARCH_X86_64_GDT_GDT_H
