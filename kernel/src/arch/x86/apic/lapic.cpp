#ifdef ARCH_X86_64
#include <arch/x86/apic/lapic.h>
#include <arch/percpu.h>
#include <sched/sched.h>
#include <ports/ports.h>
#include <memory/vmm.h>
#include <memory/paging.h>

namespace arch::x86 {
__PRIVILEGED_DATA
kstl::shared_ptr<lapic> s_system_lapics[MAX_SYSTEM_CPUS];

__PRIVILEGED_DATA
uintptr_t g_lapic_physical_base = 0;

__PRIVILEGED_DATA
volatile uint32_t* g_lapic_virtual_base = 0;

lapic::lapic(uint64_t base, uint8_t spurious_irq) {
    //
    // Since every core's LAPIC shares the same base address in
    // terms of memory mapping, the page mapping call needs to
    // only happen once.
    //
    if (!g_lapic_physical_base) {
        g_lapic_physical_base = base;

        // Map the LAPIC base into the kernel's address space
        void* virt_base = vmm::map_physical_page(base, DEFAULT_PRIV_PAGE_FLAGS | PTE_PCD);

        // Update the globally tracked virtual base pointer
        g_lapic_virtual_base = reinterpret_cast<volatile uint32_t*>(virt_base);
    }

    // Set the spurious interrupt vector
    uint32_t spurious_vector = read(0xF0);
    spurious_vector |= (1 << 8);        // Enable the APIC
    spurious_vector |= spurious_irq;    // Set the spurious interrupt vector (choose a free vector number)
    write(0xF0, spurious_vector);

    // Disable legacy PIC controller
    disable_legacy_pic();
}

__PRIVILEGED_CODE 
void lapic::write(uint32_t reg, uint32_t value) {
    g_lapic_virtual_base[reg / 4] = value;
}

__PRIVILEGED_CODE 
uint32_t lapic::read(uint32_t reg) {
    return g_lapic_virtual_base[reg / 4];
}

__PRIVILEGED_CODE 
void lapic::mask_irq(uint32_t lvtoff) {
    // Read the current LVT entry
    uint32_t lvt_entry = read(lvtoff);

    // Set the mask bit (bit 16)
    lvt_entry |= (1 << 16);

    // Write the modified LVT entry back
    write(lvtoff, lvt_entry);
}

__PRIVILEGED_CODE 
void lapic::unmask_irq(uint32_t lvtoff) {
    // Read the current LVT entry
    uint32_t lvt_entry = read(lvtoff);

    // Clear the mask bit (bit 16)
    lvt_entry &= ~(1 << 16);

    // Write the modified LVT entry back
    write(lvtoff, lvt_entry);
}

__PRIVILEGED_CODE 
void lapic::mask_timer_irq() {
    mask_irq(APIC_LVT_TIMER);
}

__PRIVILEGED_CODE 
void lapic::unmask_timer_irq() {
    unmask_irq(APIC_LVT_TIMER);
}

__PRIVILEGED_CODE 
void lapic::complete_irq() {
    write(0xB0, 0x00);
}

__PRIVILEGED_CODE 
void lapic::send_ipi(uint8_t apic_id, uint32_t vector) {
    write(APIC_ICR_HI, apic_id << 24);
    write(APIC_ICR_LO, vector | (1 << 14));
}

__PRIVILEGED_CODE 
void lapic::init() {
    int cpu = current->cpu;

    if (s_system_lapics[cpu].get() != nullptr) {
        return;
    }

    uint64_t apic_base_msr = msr::read(IA32_APIC_BASE_MSR);

    // Enable APIC by setting the 11th bit
    apic_base_msr |= (1 << 11);

    msr::write(IA32_APIC_BASE_MSR, apic_base_msr);

    uint64_t physical_base = reinterpret_cast<uint64_t>(apic_base_msr & ~0xFFF);
    s_system_lapics[cpu] = kstl::make_shared<lapic>(physical_base, 0xFF);
}

__PRIVILEGED_CODE 
kstl::shared_ptr<lapic>& lapic::get() {
    int cpu = current->cpu;

    if (s_system_lapics[cpu].get() != nullptr) {
        return s_system_lapics[cpu];
    }

    init();
    return s_system_lapics[cpu];
}

__PRIVILEGED_CODE
kstl::shared_ptr<lapic>& lapic::get(int cpu) {
    return s_system_lapics[cpu];
}

__PRIVILEGED_CODE 
void lapic::disable_legacy_pic() {
    // Send the disable command (0xFF) to both PIC1 and PIC2 data ports
    outb(0xA1, 0xFF);
    outb(0x21, 0xFF);
}
} // namespace arch::x86

#endif // ARCH_X86_64
