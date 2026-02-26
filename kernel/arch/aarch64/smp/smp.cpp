#include "arch/arch_smp.h"
#include "acpi/madt_arch.h"
#include "acpi/acpi.h"
#include "hw/psci.h"
#include "hw/cpu.h"
#include "mm/paging.h"
#include "mm/paging_types.h"
#include "mm/paging_arch.h"
#include "mm/pmm.h"
#include "mm/pmm_types.h"
#include "mm/vmm.h"
#include "mm/kva.h"
#include "percpu/percpu.h"
#include "trap/trap.h"
#include "irq/irq.h"
#include "sched/sched.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "hwtimer/hwtimer_arch.h"
#include "common/string.h"
#include "common/logging.h"

extern "C" {
    extern char asm_ap_trampoline[];
    extern char asm_ap_trampoline_end[];
}

namespace arch {

constexpr uint64_t MPIDR_AFF_MASK = 0xFF00FFFFFFULL;
constexpr uintptr_t AP_STARTUP_DATA_OFFSET = 0x100;
constexpr uint32_t AP_STACK_PAGES = 4;
constexpr uint16_t AP_GUARD_PAGES = 1;
constexpr uint32_t AP_BOOT_TIMEOUT_MS = 200;
constexpr uint32_t PERCPU_PAGES = 2;
constexpr size_t CACHE_LINE_SIZE = 64;

// ACPI FADT arm_boot_flags (offset 129, ACPI 5.1+)
constexpr size_t FADT_ARM_BOOT_FLAGS_OFFSET = 129;
constexpr uint16_t FADT_PSCI_COMPLIANT = (1 << 0);
constexpr uint16_t FADT_PSCI_USE_HVC   = (1 << 1);
constexpr uint64_t ID_AA64PFR0_EL3_SHIFT = 12;

// Startup data shared between BSP and AP (matches trampoline offsets at entry + 0x100)
struct ap_startup_data {
    uint64_t mair_el1;     // +0x00
    uint64_t tcr_el1;      // +0x08
    uint64_t ttbr1_el1;    // +0x10
    uint64_t ttbr0_el1;    // +0x18
    uint64_t sctlr_el1;    // +0x20
    uint64_t stack_top;    // +0x28
    uint64_t vbar_el1;     // +0x30
    uint64_t c_entry;      // +0x38
    uint64_t percpu_base;  // +0x40
};
static_assert(sizeof(ap_startup_data) == 72);

// Module state
static pmm::phys_addr_t g_trampoline_phys = 0;
static ap_startup_data* g_startup_data = nullptr;
static psci::conduit g_psci_conduit = psci::conduit::HVC;

static inline uint64_t read_mpidr_el1() {
    uint64_t val;
    asm volatile("mrs %0, mpidr_el1" : "=r"(val));
    return val;
}


static inline uint64_t read_id_aa64pfr0_el1() {
    uint64_t val;
    asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(val));
    return val;
}

/**
 * Clean data cache to Point of Coherency for a memory range.
 * Ensures BSP writes are visible to APs starting with D-cache off.
 */
__PRIVILEGED_CODE static void cache_clean_range(uintptr_t va, size_t len) {
    for (uintptr_t addr = va & ~(CACHE_LINE_SIZE - 1);
         addr < va + len;
         addr += CACHE_LINE_SIZE) {
        asm volatile("dc civac, %0" :: "r"(addr) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
}

/**
 * Invalidate instruction cache for a memory range.
 * Ensures AP doesn't execute stale instructions at the trampoline address.
 */
__PRIVILEGED_CODE static void icache_inval_range(uintptr_t va, size_t len) {
    for (uintptr_t addr = va & ~(CACHE_LINE_SIZE - 1);
         addr < va + len;
         addr += CACHE_LINE_SIZE) {
        asm volatile("ic ivau, %0" :: "r"(addr) : "memory");
    }
    asm volatile("dsb ish\n" "isb" ::: "memory");
}

/**
 * Build a minimal identity-map page table (L0 + L1) covering the trampoline's
 * physical address with a 1GB block descriptor.
 * @return Physical address of L0 table, or 0 on failure.
 */
__PRIVILEGED_CODE static pmm::phys_addr_t build_identity_map(pmm::phys_addr_t trampoline_phys) {
    pmm::phys_addr_t l0_phys = pmm::alloc_page(pmm::ZONE_DMA32);
    if (l0_phys == 0) return 0;

    pmm::phys_addr_t l1_phys = pmm::alloc_page(pmm::ZONE_DMA32);
    if (l1_phys == 0) {
        pmm::free_page(l0_phys);
        return 0;
    }

    auto* l0 = static_cast<uint64_t*>(paging::phys_to_virt(l0_phys));
    auto* l1 = static_cast<uint64_t*>(paging::phys_to_virt(l1_phys));

    string::memset(l0, 0, paging::PAGE_SIZE_4KB);
    string::memset(l1, 0, paging::PAGE_SIZE_4KB);

    // L0[0] = table descriptor pointing to L1
    paging::table_desc_t l0_entry{};
    l0_entry.valid = 1;
    l0_entry.type = 1;
    l0_entry.next_table_addr = l1_phys >> 12;
    l0[0] = l0_entry.value;

    // L1[n] = 1GB block descriptor (VA = PA for the trampoline's 1GB region)
    uint64_t l1_index = (trampoline_phys >> 30) & 0x1FF;
    uint64_t block_base = (trampoline_phys >> 30) << 30; // 1GB-aligned base

    paging::block_desc_t l1_entry{};
    l1_entry.valid = 1;
    l1_entry.type = 0; // block
    l1_entry.attr_idx = paging::mair_idx::NORMAL_WB;
    l1_entry.sh = paging::sh::INNER_SHAREABLE;
    l1_entry.af = 1;
    l1_entry.output_addr = block_base >> 21;
    l1[l1_index] = l1_entry.value;

    cache_clean_range(reinterpret_cast<uintptr_t>(l0), paging::PAGE_SIZE_4KB);
    cache_clean_range(reinterpret_cast<uintptr_t>(l1), paging::PAGE_SIZE_4KB);

    return l0_phys;
}

/**
 * Switch from EL1h (SPSEL=1) to EL1t (SPSEL=0).
 * Copies current SP to SP_EL0 (task stack), then sets SP_EL1 to the
 * system stack. Extracted to a separate function so the compiler
 * rebuilds stack references across the call boundary.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void switch_to_el1t(uintptr_t sys_stack_top) {
    asm volatile(
        "mov x0, sp\n\t"
        "msr sp_el0, x0\n\t"
        "mov x1, %0\n\t"
        "mov sp, x1\n\t"
        "msr spsel, #0\n\t"
        "isb"
        :: "r"(sys_stack_top) : "x0", "x1", "memory"
    );
}

// AP C entry — called from the trampoline after MMU is enabled
extern "C" __PRIVILEGED_CODE void ap_entry(uint64_t logical_id) {
    uint32_t cpu_id = static_cast<uint32_t>(logical_id);

    auto* data = static_cast<volatile ap_startup_data*>(g_startup_data);
    uintptr_t my_percpu_base = static_cast<uintptr_t>(data->percpu_base);
    uintptr_t my_stack_top = static_cast<uintptr_t>(data->stack_top);

    if (percpu::init_ap(cpu_id, my_percpu_base) != percpu::OK) {
        while (true) { asm volatile("wfi"); }
    }
    trap::load();
    irq::init_ap();

    uintptr_t sys_stack_base = 0, sys_stack_top = 0;
    if (vmm::alloc_stack(AP_STACK_PAGES, AP_GUARD_PAGES,
                         kva::tag::privileged_stack,
                         sys_stack_base, sys_stack_top) != vmm::OK) {
        smp::cpu_info* info = smp::get_cpu_info(cpu_id);
        if (info) __atomic_store_n(&info->state, smp::CPU_OFFLINE, __ATOMIC_RELEASE);
        while (true) { asm volatile("wfi"); }
    }

    switch_to_el1t(sys_stack_top);

    if (sched::init_ap(cpu_id, my_stack_top, sys_stack_top) != 0) {
        smp::cpu_info* info = smp::get_cpu_info(cpu_id);
        if (info) __atomic_store_n(&info->state, smp::CPU_OFFLINE, __ATOMIC_RELEASE);
        while (true) { asm volatile("wfi"); }
    }

    // Clear identity map from TTBR0_EL1 + TLB invalidation
    asm volatile(
        "msr ttbr0_el1, xzr\n\t"
        "dsb ishst\n\t"
        "tlbi vmalle1is\n\t"
        "dsb ish\n\t"
        "isb"
        ::: "memory"
    );

    if (clock::init_ap() != clock::OK || timer::init_ap(100) != timer::OK) {
        smp::cpu_info* info = smp::get_cpu_info(cpu_id);
        if (info) __atomic_store_n(&info->state, smp::CPU_OFFLINE, __ATOMIC_RELEASE);
        while (true) { asm volatile("wfi"); }
    }

    smp::cpu_info* info = smp::get_cpu_info(cpu_id);
    __atomic_store_n(&info->state, smp::CPU_ONLINE, __ATOMIC_RELEASE);

    while (true) { cpu::halt(); }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t smp_enumerate(smp::cpu_info* cpus, uint32_t max) {
    const acpi::madt_info& madt = acpi::get_madt_info();

    uint64_t current_mpidr = read_mpidr_el1() & MPIDR_AFF_MASK;

    uint32_t count = 0;
    for (uint32_t i = 0; i < madt.cpu_count && count < max; i++) {
        if (!madt.giccs[i].enabled) {
            continue;
        }

        uint64_t entry_mpidr = madt.giccs[i].mpidr & MPIDR_AFF_MASK;

        cpus[count].logical_id = count;
        cpus[count].hw_id = madt.giccs[i].mpidr;
        cpus[count].state = smp::CPU_OFFLINE;
        cpus[count].is_bsp = (entry_mpidr == current_mpidr);
        count++;
    }

    return count;
}

/**
 * Determine the PSCI conduit from ACPI FADT arm_boot_flags.
 * Falls back to ID_AA64PFR0_EL1 EL3 detection if FADT is unavailable.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static psci::conduit detect_psci_conduit() {
    const auto* fadt = acpi::find_table("FACP");
    if (fadt) {
        uint32_t length;
        string::memcpy(&length, &fadt->length, sizeof(length));
        if (length >= FADT_ARM_BOOT_FLAGS_OFFSET + sizeof(uint16_t)) {
            auto* base = reinterpret_cast<const uint8_t*>(fadt);
            uint16_t flags;
            string::memcpy(&flags, base + FADT_ARM_BOOT_FLAGS_OFFSET,
                           sizeof(flags));
            if (flags & FADT_PSCI_COMPLIANT) {
                return (flags & FADT_PSCI_USE_HVC)
                    ? psci::conduit::HVC
                    : psci::conduit::SMC;
            }
        }
    }

    bool has_el3 = ((read_id_aa64pfr0_el1() >> ID_AA64PFR0_EL3_SHIFT) & 0xF) != 0;
    return has_el3 ? psci::conduit::SMC : psci::conduit::HVC;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_prepare() {
    g_psci_conduit = detect_psci_conduit();
    log::info("smp: PSCI conduit: %s",
              g_psci_conduit == psci::conduit::HVC ? "HVC" : "SMC");

    // Allocate a DMA32 page for the trampoline code + startup data
    g_trampoline_phys = pmm::alloc_page(pmm::ZONE_DMA32);
    if (g_trampoline_phys == 0) {
        log::error("smp: failed to allocate trampoline page");
        return smp::ERR_PREPARE;
    }

    auto* trampoline_va = static_cast<uint8_t*>(paging::phys_to_virt(g_trampoline_phys));

    // Zero the page, then copy trampoline code
    string::memset(trampoline_va, 0, paging::PAGE_SIZE_4KB);

    size_t tramp_size = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(asm_ap_trampoline_end) -
        reinterpret_cast<uintptr_t>(asm_ap_trampoline));

    string::memcpy(trampoline_va, asm_ap_trampoline, tramp_size);

    // Build identity-map page tables for the trampoline's physical address
    pmm::phys_addr_t ttbr0_phys = build_identity_map(g_trampoline_phys);
    if (ttbr0_phys == 0) {
        log::error("smp: failed to allocate identity map tables");
        pmm::free_page(g_trampoline_phys);
        g_trampoline_phys = 0;
        return smp::ERR_PREPARE;
    }

    // Fill constant startup data fields
    g_startup_data = reinterpret_cast<ap_startup_data*>(
        trampoline_va + AP_STARTUP_DATA_OFFSET);

    g_startup_data->mair_el1 = paging::read_mair_el1();
    g_startup_data->tcr_el1 = paging::read_tcr_el1();
    g_startup_data->ttbr1_el1 = paging::get_kernel_pt_root();
    g_startup_data->ttbr0_el1 = ttbr0_phys;
    g_startup_data->sctlr_el1 = paging::read_sctlr_el1()
                                | paging::sctlr::M
                                | paging::sctlr::C
                                | paging::sctlr::I;
    g_startup_data->vbar_el1 = paging::read_vbar_el1();
    g_startup_data->c_entry = reinterpret_cast<uint64_t>(&ap_entry);

    // Cache maintenance: clean D-cache and invalidate I-cache for trampoline page
    cache_clean_range(reinterpret_cast<uintptr_t>(trampoline_va), paging::PAGE_SIZE_4KB);
    icache_inval_range(reinterpret_cast<uintptr_t>(trampoline_va), paging::PAGE_SIZE_4KB);

    return smp::OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_boot_cpu(smp::cpu_info& cpu) {
    uintptr_t percpu_va = 0;
    int32_t rc = vmm::alloc(PERCPU_PAGES, paging::PAGE_USER_RW,
                            vmm::ALLOC_ZERO, kva::tag::generic, percpu_va);
    if (rc != vmm::OK) {
        log::error("smp: failed to allocate per-CPU area for CPU %u (%d)",
                   cpu.logical_id, rc);
        return smp::ERR_BOOT_TIMEOUT;
    }

    uintptr_t stack_base = 0;
    uintptr_t stack_top = 0;
    rc = vmm::alloc_stack(AP_STACK_PAGES, AP_GUARD_PAGES,
                          kva::tag::privileged_stack,
                          stack_base, stack_top);
    if (rc != vmm::OK) {
        log::error("smp: failed to allocate stack for CPU %u (%d)",
                   cpu.logical_id, rc);
        vmm::free(percpu_va);
        return smp::ERR_BOOT_TIMEOUT;
    }

    // Fill per-AP startup data and flush to DRAM
    g_startup_data->stack_top = stack_top;
    g_startup_data->percpu_base = percpu_va;
    cache_clean_range(reinterpret_cast<uintptr_t>(g_startup_data),
                      sizeof(ap_startup_data));

    // Boot the AP via PSCI CPU_ON
    int32_t psci_rc = psci::cpu_on(g_psci_conduit, cpu.hw_id,
                                   g_trampoline_phys, cpu.logical_id);
    if (psci_rc != psci::SUCCESS) {
        log::error("smp: PSCI CPU_ON failed for CPU %u (rc=%d)",
                   cpu.logical_id, psci_rc);
        vmm::free(stack_base);
        vmm::free(percpu_va);
        return smp::ERR_BOOT_TIMEOUT;
    }

    // Poll for the AP to set CPU_ONLINE, with Generic Timer timeout
    uint64_t freq = hwtimer::read_cntfrq();
    uint64_t start = hwtimer::read_cntpct();
    uint64_t timeout_ticks = (freq * AP_BOOT_TIMEOUT_MS) / 1000;

    while (hwtimer::read_cntpct() - start < timeout_ticks) {
        if (__atomic_load_n(&cpu.state, __ATOMIC_ACQUIRE) == smp::CPU_ONLINE) {
            return smp::OK;
        }
        cpu::relax();
    }

    // AP did not come online — clean up
    vmm::free(stack_base);
    vmm::free(percpu_va);
    return smp::ERR_BOOT_TIMEOUT;
}

} // namespace arch
