#ifndef IOAPIC_H
#define IOAPIC_H
#include <ktypes.h>

#define IOAPIC_REGSEL 0x00
#define IOAPIC_IOWIN  0x10

// Initializes IOAPIC base address
void initializeIoApic();

__PRIVILEGED_CODE
void writeIoApicRegister(uint32_t reg, uint32_t value);

__PRIVILEGED_CODE
uint32_t readIoApicRegister(uint32_t reg);

__PRIVILEGED_CODE
void mapIoApicIrq(uint8_t irq, uint32_t vector, uint8_t apicId);

#endif
