#ifdef ARCH_X86_64
#ifndef LAPIC_H
#define LAPIC_H
#include <memory/memory.h>
#include <arch/x86/msr.h>

#define IA32_APIC_BASE_MSR          0x1B
#define APIC_REGISTER_SPACE_SIZE    0x400

// The offset of the ICR (Interrupt Command Register) in the Local APIC
#define APIC_ICR_LO 0x300  
#define APIC_ICR_HI 0x310

#define APIC_LVT_TIMER    0x320  // Timer interrupt
#define APIC_LVT_THERMAL  0x330  // Thermal sensor interrupt
#define APIC_LVT_PERF     0x340  // Performance monitoring interrupt
#define APIC_LVT_LINT0    0x350  // LINT0 interrupt
#define APIC_LVT_LINT1    0x360  // LINT1 interrupt
#define APIC_LVT_ERROR    0x370  // Error interrupt

namespace arch::x86 {
class lapic {
public:
    static void init();
    static kstl::shared_ptr<lapic>& get();

    lapic(uint64_t base, uint8_t spurior_irq = 0xFF);

    __PRIVILEGED_CODE void write(uint32_t reg, uint32_t value);
    __PRIVILEGED_CODE uint32_t read(uint32_t reg);

    __PRIVILEGED_CODE void mask_irq(uint32_t lvtoff);
    __PRIVILEGED_CODE void unmask_irq(uint32_t lvtoff);

    __PRIVILEGED_CODE void mask_timer_irq();
    __PRIVILEGED_CODE void unmask_timer_irq();

    __PRIVILEGED_CODE void complete_irq();
    __PRIVILEGED_CODE void send_ipi(uint8_t apic_id, uint32_t vector);

    __PRIVILEGED_CODE void disable_legacy_pic();
};
} // namespace arch::x86

#endif // LAPIC_H
#endif // ARCH_X86_64
