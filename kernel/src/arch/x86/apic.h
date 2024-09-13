#ifndef APIC_H
#define APIC_H
#include <memory/kmemory.h>

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

class Apic {
public:
    static void initializeLocalApic();
    static kstl::SharedPtr<Apic>& getLocalApic();

    Apic(uint64_t base, uint8_t spuriorIrq = 0xFF);

    void write(uint32_t reg, uint32_t value);
    uint32_t read(uint32_t reg);

    void maskIrq(uint32_t lvtoff);
    void unmaskIrq(uint32_t lvtoff);

    void maskTimerIrq();
    void unmaskTimerIrq();

    void completeIrq();
    void sendIpi(uint8_t apicId, uint32_t vector);

    static void disableLegacyPic();
};

#endif
