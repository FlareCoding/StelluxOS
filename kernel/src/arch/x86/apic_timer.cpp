#include "apic_timer.h"

ApicTimer g_apicTimer;

ApicTimer& ApicTimer::get() {
    return g_apicTimer;
}

void ApicTimer::setupPeriodic(uint8_t irqNumber, uint32_t divideConfig, uint32_t intervalValue) {
    _setup(APIC_TIMER_PERIODIC_MODE, irqNumber, divideConfig, intervalValue);
}

void ApicTimer::setupOneShot(uint8_t irqNumber, uint32_t divideConfig, uint32_t intervalValue) {
    _setup(APIC_TIMER_ONE_SHOT_MODE, irqNumber, divideConfig, intervalValue);
}

void ApicTimer::start() const {
    // To start the timer, set the initial count
    writeApicRegister(APIC_TIMER_INITIAL_COUNT, m_intervalValue);
}

uint32_t ApicTimer::readCounter() const {
    return readApicRegister(APIC_CURRENT_COUNT);
}

uint32_t ApicTimer::stop() const {
    uint32_t cnt = readCounter();

    // To stop the timer, set the initial count to 0
    writeApicRegister(APIC_TIMER_INITIAL_COUNT, 0);

    // Read the current count value
    return cnt;
}

void ApicTimer::_setup(uint32_t mode, uint8_t irqNumber, uint32_t divideConfig, uint32_t intervalValue) {
    m_irqno = irqNumber;
    m_divideConfig = divideConfig;
    m_intervalValue = intervalValue;

    // Set the timer in periodic mode
    writeApicRegister(APIC_TIMER_REGISTER, mode | irqNumber);

    // Set the divide configuration value
    writeApicRegister(APIC_TIMER_DIVIDE_CONFIG, m_divideConfig);

    // Set the timer interval value
    writeApicRegister(APIC_TIMER_INITIAL_COUNT, 0);
}
