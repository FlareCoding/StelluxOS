#include "apic.h"
#include "msr.h"
#include <paging/page.h>
#include <paging/tlb.h>
#include <ports/ports.h>
#include <kelevate/kelevate.h>

kstl::SharedPtr<Apic> s_lapic;

Apic::Apic(uint64_t base, uint8_t spuriousIrq) {
    m_physicalBase = (void*)base;

    // Map the LAPIC base into the kernel's address space
    void* virtualBase = zallocPage();

    RUN_ELEVATED({
        paging::mapPage(
            virtualBase,
            m_physicalBase,
            USERSPACE_PAGE,
            paging::g_kernelRootPageTable,
            paging::getGlobalPageFrameAllocator()
        );
        paging::flushTlbAll();
    });

    m_virtualBase = static_cast<volatile uint32_t*>(virtualBase);

    // Set the spurious interrupt vector
    uint32_t spuriousVector = read(0xF0);
    spuriousVector |= (1 << 8);    // Enable the APIC
    spuriousVector |= spuriousIrq; // Set the spurious interrupt vector (choose a free vector number)
    write(0xF0, spuriousVector);
}

void Apic::write(uint32_t reg, uint32_t value) {
    m_virtualBase[reg / 4] = value;
}

uint32_t Apic::read(uint32_t reg) {
    return m_virtualBase[reg / 4];
}

void Apic::completeIrq() {
    write(0xB0, 0x00);
}

void Apic::sendIpi(uint8_t apicId, uint32_t vector) {
    write(APIC_ICR_HI, apicId << 24);
    write(APIC_ICR_LO, vector | (1 << 14));
}

void Apic::initializeLocalApic() {
    if (s_lapic.get() != nullptr) {
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

    s_lapic = kstl::SharedPtr<Apic>(new Apic(physicalBase, 0xFF));
}

kstl::SharedPtr<Apic>& Apic::getLocalApic() {
    if (s_lapic.get() != nullptr) {
        return s_lapic;
    }

    initializeLocalApic();
    return s_lapic;
}

void Apic::disableLegacyPic() {
    RUN_ELEVATED({
        // Send the disable command (0xFF) to both PIC1 and PIC2 data ports
        outByte(0xA1, 0xFF);
        outByte(0x21, 0xFF);
    });
}
