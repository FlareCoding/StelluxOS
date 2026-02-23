#include "trap/idt.h"
#include "trap/trap.h"
#include "defs/vectors.h"
#include "hw/barrier.h"

__PRIVILEGED_BSS x86::idt_entry g_idt[256];

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void lidt(const x86::idt_ptr& idtr) {
    asm volatile("lidt %0" : : "m"(idtr) : "memory");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void set_gate(uint8_t vec, void (*handler)(), uint8_t dpl_level, uint8_t ist) {
    uint64_t addr = reinterpret_cast<uint64_t>(handler);

    x86::idt_entry& e = g_idt[vec];
    e.offset_low = static_cast<uint16_t>(addr & 0xFFFF);
    e.selector = x86::KERNEL_CS;
    e.ist = static_cast<uint8_t>(ist & 0x07); // IST bits 0-2 only; bits 3-7 are reserved and must be 0
    e.type_attr = x86::interrupt_gate_attr(dpl_level);
    e.offset_mid = static_cast<uint16_t>((addr >> 16) & 0xFFFF);
    e.offset_high = static_cast<uint32_t>((addr >> 32) & 0xFFFFFFFF);
    e.zero = 0;
}

/**
 * @brief Get IST index for a given vector.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline uint8_t get_ist_for_vector(uint8_t vec) {
    switch (vec) {
        case x86::EXC_DOUBLE_FAULT:   return x86::IST_DF;   // IST1
        case x86::EXC_NMI:            return x86::IST_NMI; // IST2
        case x86::EXC_MACHINE_CHECK:  return x86::IST_MCE; // IST3
        default:                      return 0;             // Use current stack
    }
}

extern "C" void (*stlx_x86_isr_table[256])();

namespace trap {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    for (uint16_t i = 0; i < 256; i++) {
        uint8_t vec = static_cast<uint8_t>(i);
        uint8_t dpl_level = 0;
        if (vec == x86::EXC_BREAKPOINT || vec == x86::VEC_SCHED_YIELD) {
            dpl_level = 3;
        }
        uint8_t ist = get_ist_for_vector(vec);
        set_gate(vec, stlx_x86_isr_table[i], dpl_level, ist);
    }

    // Ensure all IDT entry writes are visible before loading IDTR
    barrier::smp_write();

    const x86::idt_ptr idtr = {
        .limit = static_cast<uint16_t>(sizeof(g_idt) - 1),
        .base = reinterpret_cast<uint64_t>(&g_idt[0]),
    };

    lidt(idtr);
    return OK;
}

} // namespace trap
