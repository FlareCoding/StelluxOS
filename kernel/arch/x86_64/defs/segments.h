#ifndef STELLUX_ARCH_X86_64_DEFS_SEGMENTS_H
#define STELLUX_ARCH_X86_64_DEFS_SEGMENTS_H

#include "types.h"

namespace x86 {

// GDT entry indices
constexpr uint16_t GDT_NULL_IDX      = 0;
constexpr uint16_t GDT_KERNEL_CS_IDX = 1;
constexpr uint16_t GDT_KERNEL_DS_IDX = 2;
constexpr uint16_t GDT_USER_DS_IDX   = 3;
constexpr uint16_t GDT_USER_CS_IDX   = 4;
constexpr uint16_t GDT_TSS_IDX       = 5; // Spans indices 5-6 (16 bytes)
constexpr uint16_t GDT_ENTRIES       = 7;

// Segment selectors (index * 8, with RPL for user segments)
constexpr uint16_t NULL_SEL   = 0x00;
constexpr uint16_t KERNEL_CS  = 0x08; // GDT_KERNEL_CS_IDX * 8
constexpr uint16_t KERNEL_DS  = 0x10; // GDT_KERNEL_DS_IDX * 8
constexpr uint16_t USER_DS    = 0x1B; // (GDT_USER_DS_IDX * 8) | 3
constexpr uint16_t USER_CS    = 0x23; // (GDT_USER_CS_IDX * 8) | 3
constexpr uint16_t TSS_SEL    = 0x28; // GDT_TSS_IDX * 8

// Privilege levels
constexpr uint8_t DPL_KERNEL = 0;
constexpr uint8_t DPL_USER   = 3;

} // namespace x86

#endif // STELLUX_ARCH_X86_64_DEFS_SEGMENTS_H
