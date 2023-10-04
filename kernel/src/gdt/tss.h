#ifndef TSS_H
#define TSS_H
#include <ktypes.h>

struct TaskStateSegment {
    uint32_t reserved0;  // 0x00
    uint64_t rsp0;       // 0x04
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
    uint16_t ioMapBase;
} __attribute__((packed));

struct TSSDescriptor {
    uint16_t limitLow;
    uint16_t baseLow;
    uint8_t  baseMid;
    struct {
        uint8_t type                   : 4;    // Should be 0b1001 for 32-bit TSS and 0b1000 for 16-bit TSS
        uint8_t zero                   : 1;    // Should be zero
        uint8_t dpl                    : 2;    // Descriptor Privilege Level
        uint8_t present                : 1;
    } __attribute__((packed)) accessByte;
    struct {
        uint8_t limitHigh              : 4;
        uint8_t available              : 1;
        uint8_t zero                   : 1;    // Should be zero
        uint8_t zeroAgain              : 1;    // Should be zero
        uint8_t granularity            : 1;    // Granularity bit
    } __attribute__((packed));
    uint8_t  baseHigh;
    uint32_t baseUpper;
    uint32_t reserved;
} __attribute__((packed));

#endif
