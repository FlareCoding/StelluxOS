#include "irq/ioapic.h"
#include "acpi/madt_arch.h"
#include "hw/mmio.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "common/logging.h"

namespace ioapic {

constexpr uint32_t IOREGSEL = 0x00;
constexpr uint32_t IOWIN    = 0x10;

constexpr uint32_t IOAPIC_VER        = 0x01;
constexpr uint32_t IOAPIC_REDIR_BASE = 0x10;

// Redirection entry bits (low 32)
constexpr uint32_t REDIR_MASK     = (1u << 16);

// ISO polarity flags (ACPI spec, bits 0-1)
constexpr uint16_t POLARITY_MASK      = 0x03;
constexpr uint16_t POLARITY_ACTIVE_LOW = 0x03;

// ISO trigger flags (ACPI spec, bits 2-3)
constexpr uint16_t TRIGGER_MASK       = 0x0C;
constexpr uint16_t TRIGGER_LEVEL      = 0x0C;

__PRIVILEGED_BSS static uintptr_t g_ioapic_va;
__PRIVILEGED_BSS static uintptr_t g_ioapic_base_kva;
__PRIVILEGED_BSS static uint32_t  g_ioapic_gsi_base;
__PRIVILEGED_BSS static uint32_t  g_ioapic_max_entries;

__PRIVILEGED_CODE static void write_reg(uint8_t reg, uint32_t val) {
    mmio::write32(g_ioapic_va + IOREGSEL, reg);
    mmio::write32(g_ioapic_va + IOWIN, val);
}

__PRIVILEGED_CODE static uint32_t read_reg(uint8_t reg) {
    mmio::write32(g_ioapic_va + IOREGSEL, reg);
    return mmio::read32(g_ioapic_va + IOWIN);
}

__PRIVILEGED_CODE int32_t init() {
    const auto& madt = acpi::get_madt_info();
    if (madt.io_apic_count == 0) {
        log::warn("ioapic: no IOAPIC in MADT");
        return ERR_NONE;
    }

    const auto& io = madt.io_apics[0];
    g_ioapic_gsi_base = io.global_irq_base;

    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(io.address),
        4096,
        paging::PAGE_KERNEL_RW,
        g_ioapic_base_kva,
        g_ioapic_va);
    if (rc != vmm::OK) {
        log::error("ioapic: failed to map at 0x%x", io.address);
        return ERR_MAP;
    }

    uint32_t ver = read_reg(IOAPIC_VER);
    g_ioapic_max_entries = ((ver >> 16) & 0xFF) + 1;

    // Mask all redirection entries
    for (uint32_t i = 0; i < g_ioapic_max_entries; i++) {
        uint32_t reg_lo = IOAPIC_REDIR_BASE + i * 2;
        write_reg(reg_lo, REDIR_MASK);
        write_reg(reg_lo + 1, 0);
    }

    log::info("ioapic: initialized at 0x%x (GSI base %u, %u entries)",
              io.address, g_ioapic_gsi_base, g_ioapic_max_entries);

    return OK;
}

__PRIVILEGED_CODE int32_t route_irq(uint8_t legacy_irq, uint8_t vector,
                                    uint8_t dest_apic_id) {
    if (g_ioapic_va == 0) {
        return ERR_NONE;
    }

    const auto& madt = acpi::get_madt_info();

    uint32_t gsi = legacy_irq;
    uint16_t iso_flags = 0;

    for (uint32_t i = 0; i < madt.iso_count; i++) {
        if (madt.isos[i].source == legacy_irq) {
            gsi = madt.isos[i].global_irq;
            iso_flags = madt.isos[i].flags;
            break;
        }
    }

    if (gsi < g_ioapic_gsi_base || gsi >= g_ioapic_gsi_base + g_ioapic_max_entries) {
        log::error("ioapic: GSI %u out of range [%u, %u)",
                   gsi, g_ioapic_gsi_base, g_ioapic_gsi_base + g_ioapic_max_entries);
        return ERR_RANGE;
    }

    uint32_t entry_idx = gsi - g_ioapic_gsi_base;

    // Build low 32 bits: vector, delivery=Fixed(000), dest=Physical(0)
    uint32_t lo = vector;

    if ((iso_flags & POLARITY_MASK) == POLARITY_ACTIVE_LOW) {
        lo |= (1u << 13); // active low
    }
    if ((iso_flags & TRIGGER_MASK) == TRIGGER_LEVEL) {
        lo |= (1u << 15); // level triggered
    }

    // High 32 bits: destination APIC ID in bits 24-31
    uint32_t hi = static_cast<uint32_t>(dest_apic_id) << 24;

    uint32_t reg_lo = IOAPIC_REDIR_BASE + entry_idx * 2;
    write_reg(reg_lo + 1, hi);
    write_reg(reg_lo, lo); // unmask by not setting bit 16

    log::info("ioapic: IRQ %u -> GSI %u -> vec 0x%02x (APIC %u)",
              static_cast<uint32_t>(legacy_irq), gsi,
              static_cast<uint32_t>(vector),
              static_cast<uint32_t>(dest_apic_id));

    return OK;
}

} // namespace ioapic
