#ifdef ARCH_X86_64
#include <arch/x86/apic/apic_timer.h>

namespace arch::x86 {
// Global APIC timer controller instance
apic_timer g_apic_timer;

apic_timer& apic_timer::get() {
    return g_apic_timer;
}

__PRIVILEGED_CODE
void apic_timer::setup_periodic(uint8_t irq_number, uint32_t divide_config, uint32_t interval_value) {
    _setup(APIC_TIMER_PERIODIC_MODE, irq_number, divide_config, interval_value);
}

__PRIVILEGED_CODE
void apic_timer::setup_one_shot(uint8_t irq_number, uint32_t divide_config, uint32_t interval_value) {
    _setup(APIC_TIMER_ONE_SHOT_MODE, irq_number, divide_config, interval_value);
}

__PRIVILEGED_CODE
void apic_timer::start() const {
    // To start the timer, set the initial count
    lapic::get()->write(APIC_TIMER_INITIAL_COUNT, m_interval_value);
}

__PRIVILEGED_CODE
uint32_t apic_timer::read_counter() const {
    return lapic::get()->read(APIC_CURRENT_COUNT);
}

__PRIVILEGED_CODE
uint32_t apic_timer::stop() const {
    uint32_t cnt = read_counter();

    // To stop the timer, set the initial count to 0
    lapic::get()->write(APIC_TIMER_INITIAL_COUNT, 0);

    // Read the current count value
    return cnt;
}

__PRIVILEGED_CODE
void apic_timer::_setup(uint32_t mode, uint8_t irq_number, uint32_t divide_config, uint32_t interval_value) {
    m_irqno = irq_number;
    m_divide_config = divide_config;
    m_interval_value = interval_value;

    auto& lapic = lapic::get();

    // Set the timer in periodic mode
    lapic->write(APIC_TIMER_REGISTER, mode | irq_number);

    // Set the divide configuration value
    lapic->write(APIC_TIMER_DIVIDE_CONFIG, m_divide_config);

    // Set the timer interval value
    lapic->write(APIC_TIMER_INITIAL_COUNT, 0);
}
} // namespace arch::x86

#endif // ARCH_X86_64
