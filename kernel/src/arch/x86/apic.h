#ifndef APIC_H
#define APIC_H
#include <ktypes.h>

#define IA32_APIC_BASE_MSR          0x1B
#define APIC_REGISTER_SPACE_SIZE    0x400

// The offset of the ICR (Interrupt Command Register) in the Local APIC
#define APIC_ICR_LO 0x300  
#define APIC_ICR_HI 0x310

// Initializes and enables the APIC base address
void initializeApic();

// Returns the base address of APIC
void* getApicBase();

// Returns the physical base of local APIC
uint64_t getLocalApicPhysicalBase();

// Tell APIC that an interupt has been processed
void completeApicIrq();

void writeApicRegister(
    uint32_t reg,
    uint32_t value
);

uint32_t readApicRegister(
    uint32_t reg
);

void sendIpi(uint8_t apic_id, uint32_t vector);

#endif
