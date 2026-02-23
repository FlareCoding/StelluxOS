#ifndef STELLUX_ARCH_X86_64_DEFS_VECTORS_H
#define STELLUX_ARCH_X86_64_DEFS_VECTORS_H

#include "types.h"

namespace x86 {

// Intel-defined exception vectors (0-31)
constexpr uint8_t EXC_DIVIDE_ERROR       = 0x00;
constexpr uint8_t EXC_DEBUG              = 0x01;
constexpr uint8_t EXC_NMI                = 0x02;
constexpr uint8_t EXC_BREAKPOINT         = 0x03;
constexpr uint8_t EXC_OVERFLOW           = 0x04;
constexpr uint8_t EXC_BOUND_RANGE        = 0x05;
constexpr uint8_t EXC_INVALID_OPCODE     = 0x06;
constexpr uint8_t EXC_DEVICE_NOT_AVAIL   = 0x07;
constexpr uint8_t EXC_DOUBLE_FAULT       = 0x08;
constexpr uint8_t EXC_INVALID_TSS        = 0x0A;
constexpr uint8_t EXC_SEGMENT_NOT_PRESENT= 0x0B;
constexpr uint8_t EXC_STACK_FAULT        = 0x0C;
constexpr uint8_t EXC_GENERAL_PROTECTION = 0x0D;
constexpr uint8_t EXC_PAGE_FAULT         = 0x0E;
constexpr uint8_t EXC_X87_FPU            = 0x10;
constexpr uint8_t EXC_ALIGNMENT_CHECK    = 0x11;
constexpr uint8_t EXC_MACHINE_CHECK      = 0x12;
constexpr uint8_t EXC_SIMD_FP            = 0x13;
constexpr uint8_t EXC_VIRTUALIZATION     = 0x14;
constexpr uint8_t EXC_CONTROL_PROTECTION = 0x15;
constexpr uint8_t EXC_HYPERVISOR_INJECT  = 0x1C;
constexpr uint8_t EXC_VMM_COMMUNICATION  = 0x1D;
constexpr uint8_t EXC_SECURITY           = 0x1E;

// IRQ base (vectors 32+)
constexpr uint8_t VEC_IRQ_BASE = 0x20;

// LAPIC timer interrupt
constexpr uint8_t VEC_TIMER = 0x20;

// LAPIC spurious interrupt
constexpr uint8_t VEC_SPURIOUS = 0xFF;

// Scheduler yield (software interrupt)
constexpr uint8_t VEC_SCHED_YIELD = 0x81;

// IST (Interrupt Stack Table) indices
// IST1-IST7 are valid; 0 means use current stack
constexpr uint8_t IST_DF  = 1; // Double Fault
constexpr uint8_t IST_NMI = 2; // Non-Maskable Interrupt
constexpr uint8_t IST_MCE = 3; // Machine Check Exception

} // namespace x86

#endif // STELLUX_ARCH_X86_64_DEFS_VECTORS_H
