#include "irq/lapic.h"
#include "irq/irq.h"
#include "acpi/madt_arch.h"
#include "hw/mmio.h"
#include "hw/msr.h"
#include "hw/cpu.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "common/logging.h"

namespace irq {

constexpr uint32_t ICR_DELIVERY_BUSY = (1 << 12); // Set until the target accepts the last IPI
constexpr uint32_t ICR_DEST_SHIFT    = 24;        // Destination APIC ID in ICR_HIGH
constexpr uint32_t LAPIC_ID_SHIFT    = 24;        // xAPIC ID in LAPIC_ID

constexpr uint32_t X2APIC_MSR_BASE       = 0x800; // MSR for xAPIC offset 0
constexpr uint32_t XAPIC_REGISTER_STRIDE = 16;    // Bytes between xAPIC registers
constexpr uint32_t X2APIC_ICR_DEST_SHIFT = 32;    // Destination APIC ID in the x2APIC ICR

__PRIVILEGED_BSS static bool g_x2apic_enabled;
__PRIVILEGED_BSS static uintptr_t g_lapic_va;
__PRIVILEGED_BSS static uintptr_t g_lapic_base_kva;

// x2APIC MSR of an xAPIC register offset
static constexpr uint32_t x2apic_msr(uint32_t offset) {
    return X2APIC_MSR_BASE + offset / XAPIC_REGISTER_STRIDE;
}

// An x2APIC MSR write can pass earlier stores, so an IPI could arrive before its data
static inline void fence_before_ipi() {
    asm volatile("mfence; lfence" ::: "memory");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_lapic() {
    // Keep the firmware's mode, since some CPUs lock x2APIC on or have no xAPIC
    g_x2apic_enabled = (msr::read(MSR_IA32_APIC_BASE) & APIC_BASE_X2APIC_ENABLE) != 0;
    if (g_x2apic_enabled) {
        log::info("irq: LAPIC in x2APIC mode");
        return OK;
    }

    const auto& madt = acpi::get_madt_info();
    if (madt.lapic_base == 0) {
        log::error("irq: LAPIC base address is zero");
        return ERR_NO_MADT;
    }

    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(madt.lapic_base),
        pmm::PAGE_SIZE,
        paging::PAGE_KERNEL_RW,
        g_lapic_base_kva,
        g_lapic_va);
    if (rc != vmm::OK) {
        log::error("irq: failed to map LAPIC at 0x%lx", madt.lapic_base);
        return ERR_MAP;
    }

    log::info("irq: LAPIC in xAPIC mode at 0x%lx", madt.lapic_base);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t read_lapic_register(uint32_t offset) {
    if (g_x2apic_enabled) {
        return static_cast<uint32_t>(msr::read(x2apic_msr(offset)));
    }

    return mmio::read32(g_lapic_va + offset);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void write_lapic_register(uint32_t offset, uint32_t value) {
    if (g_x2apic_enabled) {
        msr::write(x2apic_msr(offset), value);
        return;
    }

    mmio::write32(g_lapic_va + offset, value);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t read_lapic_id() {
    if (g_x2apic_enabled) {
        return read_lapic_register(LAPIC_ID);
    }

    return read_lapic_register(LAPIC_ID) >> LAPIC_ID_SHIFT;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void wait_icr_idle() {
    while (read_lapic_register(LAPIC_ICR_LOW) & ICR_DELIVERY_BUSY) {
        cpu::relax();
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void send_lapic_ipi(uint32_t apic_id, uint32_t command) {
    // x2APIC sends the whole IPI in one MSR write with no busy bit to poll
    if (g_x2apic_enabled) {
        uint64_t icr = (static_cast<uint64_t>(apic_id) << X2APIC_ICR_DEST_SHIFT) | command;
        fence_before_ipi();
        msr::write(x2apic_msr(LAPIC_ICR_LOW), icr);
        return;
    }

    // The destination and command halves must land as a pair, an interrupt
    // handler sending its own IPI between them would redirect this one.
    uint64_t flags = cpu::irq_save();
    wait_icr_idle();

    write_lapic_register(LAPIC_ICR_HIGH, apic_id << ICR_DEST_SHIFT);
    write_lapic_register(LAPIC_ICR_LOW, command);

    cpu::irq_restore(flags);
}

} // namespace irq
