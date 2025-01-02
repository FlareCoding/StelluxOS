#ifdef ARCH_X86_64
#include <arch/x86/apic/ioapic.h>
#include <sched/sched.h>
#include <memory/vmm.h>
#include <memory/paging.h>

namespace arch::x86 {
__PRIVILEGED_DATA kstl::shared_ptr<ioapic> g_primary_ioapic_instance;

__PRIVILEGED_CODE
kstl::shared_ptr<ioapic>& ioapic::get() {
    return g_primary_ioapic_instance;
}

__PRIVILEGED_CODE
void ioapic::create(uint64_t physbase, uint64_t gsib) {
    g_primary_ioapic_instance = kstl::make_shared<ioapic>(physbase, gsib);
}

__PRIVILEGED_CODE
ioapic::ioapic(uint64_t phys_regs, uint64_t gsib) {
    m_physical_base = phys_regs;
    m_global_intr_base = gsib;
    m_virtual_base = reinterpret_cast<uintptr_t>(
        vmm::map_contiguous_physical_pages(m_physical_base, 2, DEFAULT_PRIV_PAGE_FLAGS | PTE_PCD)
    );

    // Initialize APIC ID and version
    m_apic_id = (_read(IOAPICID) >> 24) & 0xF0;
    m_apic_version = _read(IOAPICVER);

    // Initialize redirection entry count and global interrupt base
    m_redirection_entry_count = (_read(IOAPICVER) >> 16) + 1;
    m_global_intr_base = gsib;
}

__PRIVILEGED_CODE
ioapic::redirection_entry ioapic::get_redirection_entry(uint8_t ent_no) const {
    // Check if the entry number is within the valid range
    if (ent_no >= m_redirection_entry_count) {
        redirection_entry empty_entry;
        memset(&empty_entry, 0, sizeof(redirection_entry));
        return empty_entry;
    }

    redirection_entry entry;
    // Read the lower and upper 32-bits of the redirection entry
    entry.lower_dword = _read(IOAPICREDTBL(ent_no));
    entry.upper_dword = _read(IOAPICREDTBL(ent_no) + 1);

    return entry;
}

__PRIVILEGED_CODE
bool ioapic::write_redirection_entry(uint8_t ent_no, const redirection_entry* entry) {
    // Check if the entry number is within the valid range
    if (ent_no >= m_redirection_entry_count) {
        return false;
    }

    // Write the lower and upper 32-bits of the redirection entry
    _write(IOAPICREDTBL(ent_no), entry->lower_dword);
    _write(IOAPICREDTBL(ent_no) + 1, entry->upper_dword);

    return true;
}

__PRIVILEGED_CODE
uint32_t ioapic::_read(uint8_t reg_off) const {
    uint32_t result = 0;

    *reinterpret_cast<volatile uint32_t*>(m_virtual_base + IOAPIC_REGSEL) = reg_off;
    result = *reinterpret_cast<volatile uint32_t*>(m_virtual_base + IOAPIC_IOWIN);

    return result;
}

void ioapic::_write(uint8_t reg_off, uint32_t data) {
    *reinterpret_cast<volatile uint32_t*>(m_virtual_base + IOAPIC_REGSEL) = reg_off;
    *reinterpret_cast<volatile uint32_t*>(m_virtual_base + IOAPIC_IOWIN) = data;
}
} // namespace arch::x86

__PRIVILEGED_CODE
void route_legacy_irq(uint8_t irq_line, uint8_t irqno, uint8_t cpu = 0, uint8_t level_triggered = 0) {
    auto& io_apic = arch::x86::ioapic::get();

    arch::x86::ioapic::redirection_entry entry;
    zeromem(&entry, sizeof(arch::x86::ioapic::redirection_entry));

    entry.vector = irqno;
    entry.destination = cpu;
    entry.trigger_mode = level_triggered;
    io_apic->write_redirection_entry(irq_line, &entry);
}

#endif // ARCH_X86_64
