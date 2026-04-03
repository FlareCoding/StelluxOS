#ifndef STELLUX_ARCH_X86_64_TRAP_IDT_H
#define STELLUX_ARCH_X86_64_TRAP_IDT_H

#include "common/types.h"
#include "defs/segments.h"

namespace x86 {

// 64-bit IDT entry (interrupt/trap gate).
struct __attribute__((packed)) idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
};

struct __attribute__((packed)) idt_ptr {
    uint16_t limit;
    uint64_t base;
};

static_assert(sizeof(idt_entry) == 16, "idt_entry must be 16 bytes");
static_assert(sizeof(idt_ptr) == 10, "idt_ptr must be 10 bytes");

constexpr uint8_t GATE_TYPE_INTERRUPT = 0x0E;
constexpr uint8_t PRESENT = 0x80;

constexpr uint8_t dpl(uint8_t level) {
    return static_cast<uint8_t>((level & 0x3) << 5);
}

constexpr uint8_t interrupt_gate_attr(uint8_t dpl_level) {
    return static_cast<uint8_t>(PRESENT | dpl(dpl_level) | GATE_TYPE_INTERRUPT);
}

} // namespace x86

#endif // STELLUX_ARCH_X86_64_TRAP_IDT_H
