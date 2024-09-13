#include "apic.h"
#include "msr.h"
#include <paging/page.h>
#include <paging/tlb.h>
#include <ports/ports.h>
#include <kelevate/kelevate.h>
#include <arch/x86/per_cpu_data.h>

kstl::SharedPtr<Apic> s_lapics[MAX_CPUS];

void*               g_lapicPhysicalBase = nullptr;
volatile uint32_t*  g_lapicVirtualBase = nullptr;

Apic::Apic(uint64_t base, uint8_t spuriousIrq) {
    //
    // Since every core's LAPIC shares the same base address in
    // terms of memory mapping, the page mapping call needs to
    // only happen once.
    //
    if (!g_lapicPhysicalBase) {
        g_lapicPhysicalBase = (void*)base;

        // Map the LAPIC base into the kernel's address space
        g_lapicVirtualBase = (volatile uint32_t*)zallocPage();

        RUN_ELEVATED({
            paging::mapPage(
                (void*)g_lapicVirtualBase,
                g_lapicPhysicalBase,
                USERSPACE_PAGE,
                PAGE_ATTRIB_CACHE_DISABLED,
                paging::g_kernelRootPageTable,
                paging::getGlobalPageFrameAllocator()
            );
            
            paging::flushTlbPage((void*)g_lapicVirtualBase);
        });
    }

    // Set the spurious interrupt vector
    uint32_t spuriousVector = read(0xF0);
    spuriousVector |= (1 << 8);    // Enable the APIC
    spuriousVector |= spuriousIrq; // Set the spurious interrupt vector (choose a free vector number)
    write(0xF0, spuriousVector);
}

void Apic::write(uint32_t reg, uint32_t value) {
    g_lapicVirtualBase[reg / 4] = value;
}

uint32_t Apic::read(uint32_t reg) {
    return g_lapicVirtualBase[reg / 4];
}

void Apic::maskIrq(uint32_t lvtoff) {
    // Read the current LVT entry
    uint32_t lvtEntry = read(lvtoff);

    // Set the mask bit (bit 16)
    lvtEntry |= (1 << 16);

    // Write the modified LVT entry back
    write(lvtoff, lvtEntry);
}

void Apic::unmaskIrq(uint32_t lvtoff) {
    // Read the current LVT entry
    uint32_t lvtEntry = read(lvtoff);

    // Clear the mask bit (bit 16)
    lvtEntry &= ~(1 << 16);

    // Write the modified LVT entry back
    write(lvtoff, lvtEntry);
}

void Apic::maskTimerIrq() {
    maskIrq(APIC_LVT_TIMER);
}

void Apic::unmaskTimerIrq() {
    unmaskIrq(APIC_LVT_TIMER);
}

void Apic::completeIrq() {
    write(0xB0, 0x00);
}

void Apic::sendIpi(uint8_t apicId, uint32_t vector) {
    write(APIC_ICR_HI, apicId << 24);
    write(APIC_ICR_LO, vector | (1 << 14));
}

void Apic::initializeLocalApic() {
    int cpu = getCurrentCpuId();

    if (s_lapics[cpu].get() != nullptr) {
        return;
    }

    uint64_t apicBaseMsr = 0;

    RUN_ELEVATED({
        apicBaseMsr = readMsr(IA32_APIC_BASE_MSR);

        // Enable APIC by setting the 11th bit
        apicBaseMsr |= (1 << 11);

        writeMsr(IA32_APIC_BASE_MSR, apicBaseMsr);
    });

    uint64_t physicalBase = (uint64_t)(apicBaseMsr & ~0xFFF);

    s_lapics[cpu] = kstl::SharedPtr<Apic>(new Apic(physicalBase, 0xFF));
}

kstl::SharedPtr<Apic>& Apic::getLocalApic() {
    int cpu = getCurrentCpuId();

    if (s_lapics[cpu].get() != nullptr) {
        return s_lapics[cpu];
    }

    initializeLocalApic();
    return s_lapics[cpu];
}

void Apic::disableLegacyPic() {
    RUN_ELEVATED({
        // Send the disable command (0xFF) to both PIC1 and PIC2 data ports
        outByte(0xA1, 0xFF);
        outByte(0x21, 0xFF);
    });
}
