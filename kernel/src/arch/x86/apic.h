#ifndef APIC_H
#define APIC_H
#include <memory/kmemory.h>

#define IA32_APIC_BASE_MSR          0x1B
#define APIC_REGISTER_SPACE_SIZE    0x400

// The offset of the ICR (Interrupt Command Register) in the Local APIC
#define APIC_ICR_LO 0x300  
#define APIC_ICR_HI 0x310

class Apic {
public:
    static void initializeLocalApic();
    static kstl::SharedPtr<Apic>& getLocalApic();

    //
    // Meant to only be called from the interrupt
    // context to avoid incorrectly lowering.
    // This is because of the call to retrieve cpu core's id.
    //
    __PRIVILEGED_CODE static kstl::SharedPtr<Apic>& __irqGetLocalApic();

    Apic(uint64_t base, uint8_t spuriorIrq = 0xFF);

    void write(uint32_t reg, uint32_t value);
    uint32_t read(uint32_t reg);

    void completeIrq();
    void sendIpi(uint8_t apicId, uint32_t vector);

    static void disableLegacyPic();
};

#endif
