#include "apic.h"
#include "msr.h"
#include <paging/page.h>
#include <memory/kmemory.h>
#include <paging/tlb.h>

volatile uint32_t* g_lapicBase = NULL;

void initializeApic() {
    if (g_lapicBase != NULL) {
        return;
    }

    uint64_t apicBaseMsr = readMsr(IA32_APIC_BASE_MSR);

    // Enable APIC by setting the 11th bit
    apicBaseMsr |= (1 << 11);
    writeMsr(IA32_APIC_BASE_MSR, apicBaseMsr);

    g_lapicBase = (uint32_t*)(apicBaseMsr & ~0xFFF);

    // Map the LAPIC base into the kernel's address space
    void* lapicVirtualBase = zallocPage(); // this will lock a kernel page for our use

    paging::mapPage(
        lapicVirtualBase,
        (void*)g_lapicBase,
        KERNEL_PAGE,
        paging::g_kernelRootPageTable,
        paging::getGlobalPageFrameAllocator()
    );
    paging::flushTlbAll();

    g_lapicBase = (uint32_t*)lapicVirtualBase;

    // Set the spurious interrupt vector and enable the APIC
    uint32_t spurious_vector = readApicRegister(0xF0);
    spurious_vector |= (1 << 8);    // Enable the APIC
    spurious_vector |= 0xFF;        // Set the spurious interrupt vector (choose a free vector number)
    writeApicRegister(0xF0, spurious_vector);
}

void* getApicBase() {
    return (void*)g_lapicBase;
}

void configureApicTimerIrq(
    uint8_t irqno
) {
    // Set the timer in periodic mode
    writeApicRegister(0x320, 0x20000 | irqno);

    // Set the divide configuration value
    writeApicRegister(0x3E0, APIC_TIMER_DIVIDE_CONFIG);

    // Set the timer interval value
    writeApicRegister(0x380, APIC_TIMER_INTERVAL_VALUE);
}

void completeApicIrq() {
    writeApicRegister(0xB0, 0);
}

void writeApicRegister(
    uint32_t reg,
    uint32_t value
) {
    g_lapicBase[reg / 4] = value;
}

uint32_t readApicRegister(
    uint32_t reg
) {
    return g_lapicBase[reg / 4];
}

void sendIpi(uint8_t apic_id, uint32_t vector) {
    writeApicRegister(APIC_ICR_HI, apic_id << 24);
    writeApicRegister(APIC_ICR_LO, vector | (1 << 14));
}
