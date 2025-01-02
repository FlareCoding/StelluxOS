#ifdef ARCH_X86_64
#ifndef TSS_H
#define TSS_H
#include <types.h>

struct task_state_segment {
    uint32_t reserved0;  // 0x00
    uint64_t rsp0;       // 0x04 (x86_64 ring0 stack)
    uint64_t rsp1;       // 0x0C
    uint64_t rsp2;       // 0x14
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
    uint16_t io_map_base;
} __attribute__((packed));

struct tss_desc {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    struct {
        uint8_t type                   : 4;    // Should be 0b1001 for 32-bit TSS and 0b1000 for 16-bit TSS
        uint8_t zero                   : 1;    // Should be zero
        uint8_t dpl                    : 2;    // Descriptor Privilege Level
        uint8_t present                : 1;
    } __attribute__((packed)) access_byte;
    struct {
        uint8_t limit_high             : 4;
        uint8_t available              : 1;
        uint8_t zero                   : 1;    // Should be zero
        uint8_t zero_again             : 1;    // Should be zero
        uint8_t granularity            : 1;    // Granularity bit
    } __attribute__((packed));
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

#endif // TSS_H
#endif // ARCH_X86_64
