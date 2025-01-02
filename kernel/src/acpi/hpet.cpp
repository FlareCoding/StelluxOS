#include <acpi/hpet.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <memory/tlb.h>

namespace acpi {
// Global system instance of HPET controller
hpet g_hpet;

hpet& hpet::get() {
    return g_hpet;
}

__PRIVILEGED_CODE
void hpet::init(acpi_sdt_header* acpi_hpet_table) {
    hpet_table* table = reinterpret_cast<hpet_table*>(acpi_hpet_table);

    // Retrieve the physical HPET base from the ACPI table
    uintptr_t physical_base = table->address;

    // Map the HPET controller into the kernel's virtual address space
    void* virt_base = vmm::map_physical_page(physical_base, DEFAULT_UNPRIV_PAGE_FLAGS | PTE_PCD);

    m_base = reinterpret_cast<uint64_t>(virt_base);

    // TLB has to be flushed for proper writes to HPET registers in the future
    paging::tlb_flush_all();

    // Enable the HPET by setting the ENABLE bit in the General Configuration Register
    uint64_t gen_config = _read_hpet_register(HPET_GENERAL_CONFIGURATION_OFFSET);
    gen_config |= HPET_ENABLE_BIT;
    _write_hpet_register(HPET_GENERAL_CONFIGURATION_OFFSET, gen_config);
}

uint64_t hpet::read_counter() {
    return _read_hpet_register(HPET_MAIN_COUNTER_OFFSET);
}

uint64_t hpet::qeuery_frequency() const {
    uint64_t gc_id_reg = _read_hpet_register(HPET_GENERAL_CAPABILITIES_ID_REGISTER);
    uint32_t clock_period_fs = static_cast<uint32_t>(gc_id_reg >> 32);  // The upper 32 bits contain the period

    if (clock_period_fs == 0) {
        return 0;
    }

    // Convert the period from femtoseconds to Hz
    return 1000000000000000ULL / clock_period_fs;
}

uint64_t hpet::_read_hpet_register(uint64_t offset) const {
    return *reinterpret_cast<volatile uint64_t*>(m_base + offset);
}

void hpet::_write_hpet_register(uint64_t offset, uint64_t value) {
    *reinterpret_cast<volatile uint64_t*>(m_base + offset) = value;
}
} // namespace acpi
