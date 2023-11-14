#ifndef APIC_TIMER_H
#define APIC_TIMER_H
#include "apic.h"

// Macros for APIC Timer Registers and Configurations
#define APIC_TIMER_REGISTER        0x320
#define APIC_TIMER_DIVIDE_CONFIG   0x3E0
#define APIC_TIMER_INITIAL_COUNT   0x380
#define APIC_CURRENT_COUNT         0x390

#define APIC_TIMER_ONE_SHOT_MODE   0x0
#define APIC_TIMER_PERIODIC_MODE   0x20000

class ApicTimer {
public:
    static ApicTimer& get();

    ApicTimer() = default;

    void setupPeriodic(uint8_t irqNumber, uint32_t divideConfig, uint32_t intervalValue);
    void setupOneShot(uint8_t irqNumber, uint32_t divideConfig, uint32_t intervalValue);

    void start() const;
    uint32_t readCounter() const;
    uint32_t stop() const;

private:
    uint8_t     m_irqno;
    uint32_t    m_divideConfig;
    uint32_t    m_intervalValue;

    void _setup(uint32_t mode, uint8_t irqNumber, uint32_t divideConfig, uint32_t intervalValue);
};

#endif
