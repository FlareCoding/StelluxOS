#include "arch/arch_smp.h"
#include "acpi/madt_arch.h"
#include "irq/irq_arch.h"
#include "hw/mmio.h"
#include "hw/msr.h"
#include "hw/cpu.h"
#include "hw/delay.h"
#include "mm/paging.h"
#include "mm/paging_types.h"
#include "mm/vmm.h"
#include "mm/kva.h"
#include "common/string.h"
#include "common/logging.h"

extern "C" {
    extern char asm_ap_trampoline[];
    extern char asm_ap_trampoline_end[];
}

namespace arch {

// Physical addresses for the trampoline and startup data
constexpr uintptr_t AP_TRAMPOLINE_PHYS = 0x8000;
constexpr uintptr_t AP_STARTUP_DATA_PHYS = 0x9000;
constexpr uint32_t  AP_SIPI_VECTOR = AP_TRAMPOLINE_PHYS >> 12; // 0x8

constexpr uint32_t AP_STACK_PAGES = 4;
constexpr uint16_t AP_GUARD_PAGES = 1;

// LAPIC ICR command constants
constexpr uint32_t ICR_DM_INIT       = (5 << 8);
constexpr uint32_t ICR_DM_STARTUP    = (6 << 8);
constexpr uint32_t ICR_LEVEL_ASSERT  = (1 << 14);
constexpr uint32_t ICR_TRIGGER_LEVEL = (1 << 15);
constexpr uint32_t ICR_DELIVERY_BUSY = (1 << 12);
constexpr uint32_t ICR_DEST_SHIFT    = 24;

// IPI timing
constexpr uint32_t IPI_INIT_DELAY_MS    = 10;
constexpr uint32_t IPI_SIPI_DELAY_MS    = 1;
constexpr uint32_t IPI_TIMEOUT_MS       = 200;

// Startup data shared between BSP and AP (matches trampoline offsets at 0x9000)
struct ap_startup_data {
    uint64_t page_table_phys; // +0x00
    uint64_t stack_top;       // +0x08
    uint64_t logical_id;      // +0x10
    uint64_t c_entry;         // +0x18
};
static_assert(sizeof(ap_startup_data) == 32);

constexpr uint32_t MSR_IA32_APIC_BASE = 0x1B;
constexpr uint64_t APIC_BASE_BSP_FLAG = (1ULL << 8);

// AP C entry — called from the trampoline's 64-bit section
extern "C" __PRIVILEGED_CODE void ap_entry(uint64_t logical_id) {
    smp::cpu_info* info = smp::get_cpu_info(static_cast<uint32_t>(logical_id));
    if (info) {
        __atomic_store_n(&info->state, smp::CPU_ONLINE, __ATOMIC_RELEASE);
    }
    while (true) {
        asm volatile("cli; hlt");
    }
}

__PRIVILEGED_CODE static void wait_icr_idle() {
    uintptr_t lapic_va = irq::get_lapic_va();
    while (mmio::read32(lapic_va + irq::LAPIC_ICR_LOW) & ICR_DELIVERY_BUSY) {
        cpu::relax();
    }
}

__PRIVILEGED_CODE static void send_init_ipi(uint8_t apic_id) {
    uintptr_t lapic_va = irq::get_lapic_va();

    // Assert INIT
    mmio::write32(lapic_va + irq::LAPIC_ICR_HIGH,
                  static_cast<uint32_t>(apic_id) << ICR_DEST_SHIFT);
    mmio::write32(lapic_va + irq::LAPIC_ICR_LOW,
                  ICR_DM_INIT | ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);
    wait_icr_idle();

    // Deassert INIT
    mmio::write32(lapic_va + irq::LAPIC_ICR_HIGH,
                  static_cast<uint32_t>(apic_id) << ICR_DEST_SHIFT);
    mmio::write32(lapic_va + irq::LAPIC_ICR_LOW,
                  ICR_DM_INIT | ICR_TRIGGER_LEVEL);
    wait_icr_idle();
}

__PRIVILEGED_CODE static void send_startup_ipi(uint8_t apic_id, uint32_t vector) {
    uintptr_t lapic_va = irq::get_lapic_va();

    mmio::write32(lapic_va + irq::LAPIC_ICR_HIGH,
                  static_cast<uint32_t>(apic_id) << ICR_DEST_SHIFT);
    mmio::write32(lapic_va + irq::LAPIC_ICR_LOW,
                  (vector & 0xFF) | ICR_DM_STARTUP);
    wait_icr_idle();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t smp_enumerate(smp::cpu_info* cpus, uint32_t max) {
    const acpi::madt_info& madt = acpi::get_madt_info();

    uint8_t bsp_apic_id = 0;
    uint64_t apic_base_msr = msr::read(MSR_IA32_APIC_BASE);
    if (apic_base_msr & APIC_BASE_BSP_FLAG) {
        uintptr_t lapic_va = irq::get_lapic_va();
        bsp_apic_id = static_cast<uint8_t>(mmio::read32(lapic_va + irq::LAPIC_ID) >> 24);
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < madt.lapic_count && count < max; i++) {
        if (!madt.lapics[i].enabled) {
            continue;
        }

        cpus[count].logical_id = count;
        cpus[count].hw_id = madt.lapics[i].apic_id;
        cpus[count].state = smp::CPU_OFFLINE;
        cpus[count].is_bsp = (madt.lapics[i].apic_id == bsp_apic_id);
        count++;
    }

    return count;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_prepare() {
    pmm::phys_addr_t pt_root = paging::get_kernel_pt_root();

    if (pt_root >= 0x100000000ULL) {
        log::error("smp: PML4 at 0x%lx is above 4GB, cannot boot APs", pt_root);
        return smp::ERR_PREPARE;
    }

    // Identity-map the trampoline code page and startup data page
    int32_t rc = paging::map_page(AP_TRAMPOLINE_PHYS, AP_TRAMPOLINE_PHYS,
                                  paging::PAGE_KERNEL_RWX, pt_root);
    if (rc != paging::OK && rc != paging::ERR_ALREADY_MAPPED) {
        log::error("smp: failed to identity-map trampoline at 0x%lx (%d)",
                   AP_TRAMPOLINE_PHYS, rc);
        return smp::ERR_PREPARE;
    }

    rc = paging::map_page(AP_STARTUP_DATA_PHYS, AP_STARTUP_DATA_PHYS,
                          paging::PAGE_KERNEL_RW, pt_root);
    if (rc != paging::OK && rc != paging::ERR_ALREADY_MAPPED) {
        log::error("smp: failed to identity-map startup data at 0x%lx (%d)",
                   AP_STARTUP_DATA_PHYS, rc);
        return smp::ERR_PREPARE;
    }

    paging::flush_tlb_page(AP_TRAMPOLINE_PHYS);
    paging::flush_tlb_page(AP_STARTUP_DATA_PHYS);

    // Copy trampoline code to physical 0x8000
    size_t tramp_size = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(asm_ap_trampoline_end) -
        reinterpret_cast<uintptr_t>(asm_ap_trampoline));

    string::memcpy(reinterpret_cast<void*>(AP_TRAMPOLINE_PHYS),
                   asm_ap_trampoline, tramp_size);

    // Initialize startup data fields that are constant across all APs
    auto* data = reinterpret_cast<ap_startup_data*>(AP_STARTUP_DATA_PHYS);
    data->page_table_phys = pt_root;
    data->c_entry = reinterpret_cast<uint64_t>(&ap_entry);

    return smp::OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_boot_cpu(smp::cpu_info& cpu) {
    uintptr_t stack_base = 0;
    uintptr_t stack_top = 0;
    int32_t rc = vmm::alloc_stack(AP_STACK_PAGES, AP_GUARD_PAGES,
                                  kva::tag::privileged_stack,
                                  stack_base, stack_top);
    if (rc != vmm::OK) {
        log::error("smp: failed to allocate stack for CPU %u (%d)",
                   cpu.logical_id, rc);
        return smp::ERR_BOOT_TIMEOUT;
    }

    // Fill per-AP fields in startup data
    auto* data = reinterpret_cast<ap_startup_data*>(AP_STARTUP_DATA_PHYS);
    data->stack_top = stack_top;
    data->logical_id = cpu.logical_id;

    uint8_t apic_id = static_cast<uint8_t>(cpu.hw_id);

    // INIT-SIPI-SIPI sequence
    send_init_ipi(apic_id);
    delay::pit_ms(IPI_INIT_DELAY_MS);

    send_startup_ipi(apic_id, AP_SIPI_VECTOR);
    delay::pit_ms(IPI_SIPI_DELAY_MS);

    // Check if AP came online
    if (__atomic_load_n(&cpu.state, __ATOMIC_ACQUIRE) == smp::CPU_ONLINE) {
        return smp::OK;
    }

    // Send second SIPI and poll with timeout
    send_startup_ipi(apic_id, AP_SIPI_VECTOR);

    for (uint32_t waited = 0; waited < IPI_TIMEOUT_MS; waited++) {
        delay::pit_ms(1);
        if (__atomic_load_n(&cpu.state, __ATOMIC_ACQUIRE) == smp::CPU_ONLINE) {
            return smp::OK;
        }
    }

    // AP did not come online — clean up
    vmm::free(stack_base);
    return smp::ERR_BOOT_TIMEOUT;
}

} // namespace arch
