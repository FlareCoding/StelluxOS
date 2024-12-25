#ifdef ARCH_X86_64
#ifndef LAPIC_TIMER_H
#define LAPIC_TIMER_H
#include "lapic.h"

// Macros for APIC Timer Registers and Configurations
#define APIC_TIMER_REGISTER        0x320
#define APIC_TIMER_DIVIDE_CONFIG   0x3E0
#define APIC_TIMER_INITIAL_COUNT   0x380
#define APIC_CURRENT_COUNT         0x390

#define APIC_TIMER_ONE_SHOT_MODE   0x0
#define APIC_TIMER_PERIODIC_MODE   0x20000

namespace arch::x86 {
class apic_timer {
public:
    static apic_timer& get();

    apic_timer() = default;

    __PRIVILEGED_CODE void setup_periodic(uint8_t irq_number, uint32_t divide_config, uint32_t interval_value);
    __PRIVILEGED_CODE void setup_one_shot(uint8_t irq_number, uint32_t divide_config, uint32_t interval_value);

    __PRIVILEGED_CODE void start() const;
    __PRIVILEGED_CODE uint32_t read_counter() const;
    __PRIVILEGED_CODE uint32_t stop() const;

private:
    uint8_t     m_irqno;
    uint32_t    m_divide_config;
    uint32_t    m_interval_value;

    __PRIVILEGED_CODE void _setup(uint32_t mode, uint8_t irq_number, uint32_t divide_config, uint32_t interval_value);
};
} // namespace arch::x86

#endif // LAPIC_TIMER_H
#endif // ARCH_X86_64
