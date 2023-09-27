#ifndef APIC_H
#define APIC_H
#include <ktypes.h>

#define IA32_APIC_BASE_MSR          0x1B
#define APIC_REGISTER_SPACE_SIZE    0x400
#define APIC_TIMER_DIVIDE_CONFIG    0x3
#define APIC_TIMER_INTERVAL_VALUE   0x10000

// Initializes and enables the APIC base address
void initializeApic();

// Returns the base address of APIC
void* getApicBase();

// Configures the APIC timer interrupt
void configureApicTimerIrq(
    uint8_t irqno
);

// Tell APIC that an interupt has been processed
void completeApicIrq();

void writeApicRegister(
    uint32_t reg,
    uint32_t value
);

uint32_t readApicRegister(
    uint32_t reg
);


#endif
