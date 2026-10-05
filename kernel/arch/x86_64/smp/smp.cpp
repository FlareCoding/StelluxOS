#include "arch/arch_smp.h"
#include "acpi/madt_arch.h"
#include "irq/lapic.h"
#include "irq/irq.h"
#include "smp/ipi.h"
#include "defs/vectors.h"
#include "hw/msr.h"
#include "hw/delay.h"
#include "mm/paging.h"
#include "mm/paging_types.h"
#include "mm/vmm.h"
#include "mm/kva.h"
#include "common/string.h"
#include "common/logging.h"
#include "percpu/percpu.h"
#include "gdt/gdt.h"
#include "trap/trap.h"
#include "hw/cpu_features.h"
#include "syscall/syscall.h"
#include "sched/sched.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "trace/ktrace.h"

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

// IPI timing
constexpr uint32_t IPI_INIT_DELAY_MS    = 10;
constexpr uint32_t IPI_SIPI_DELAY_MS    = 1;
constexpr uint32_t IPI_TIMEOUT_MS       = 400;

constexpr uint32_t PERCPU_PAGES = 2;

// Startup data shared between BSP and AP (matches trampoline offsets at 0x9000)
struct ap_startup_data {
    uint64_t page_table_phys; // +0x00
    uint64_t stack_top;       // +0x08
    uint64_t logical_id;      // +0x10
    uint64_t c_entry;         // +0x18
    uint64_t percpu_base;     // +0x20 (per-CPU area VA, read by ap_entry)
};
static_assert(sizeof(ap_startup_data) == 40);

// AP C entry, called from the trampoline's 64-bit section
extern "C" __PRIVILEGED_CODE void ap_entry(uint64_t logical_id) {
    auto* data = reinterpret_cast<ap_startup_data*>(AP_STARTUP_DATA_PHYS);
    uint32_t cpu_id = static_cast<uint32_t>(logical_id);

    // Claim before touching the per-CPU area or running any init, so a CPU the
    // BSP already gave up on parks instead of using memory it no longer owns.
    smp::cpu_info* info = smp::get_cpu_info(cpu_id);
    uint32_t claim = smp::CPU_BOOTING;
    if (!info || !info->state.cmpxchg_strong_acq_rel(claim, smp::CPU_CLAIMED)) {
        while (true) { asm volatile("cli; hlt"); }
    }

    // Per-CPU area first, enables this_cpu() for everything else
    percpu::init_ap(cpu_id, data->percpu_base);

    // Allocate IST stacks (VMM safe after GS set, spinlocks use pushfq/cli)
    uintptr_t ist1_base = 0, ist1_top = 0;
    uintptr_t ist2_base = 0, ist2_top = 0;
    uintptr_t ist3_base = 0, ist3_top = 0;
    if (vmm::alloc_stack(2, 0, kva::tag::privileged_stack, ist1_base, ist1_top) != vmm::OK ||
        vmm::alloc_stack(2, 0, kva::tag::privileged_stack, ist2_base, ist2_top) != vmm::OK ||
        vmm::alloc_stack(2, 0, kva::tag::privileged_stack, ist3_base, ist3_top) != vmm::OK) {
        while (true) { asm volatile("cli; hlt"); }
    }

    // GDT/TSS, uses this_cpu() for per-CPU GDT/TSS structs
    x86::gdt::init(data->stack_top, ist1_top, ist2_top, ist3_top);
    x86::gdt::load();

    // Load shared IDT (lidt only, no rebuild)
    trap::load();

    // CPU features (CR4.FSGSBASE, PAT MSR, per-CPU registers)
    cpu::init();

    // Syscall MSRs (LSTAR/STAR/SFMASK, per-CPU)
    syscall::init_arch_syscalls();

    // LAPIC enable (SVR, mask LVTs, clear EOI, per-CPU hardware)
    irq::init_ap();
    smp::ipi::init_ap();

    // Allocate a separate system stack for the idle task.
    uintptr_t sys_stack_base = 0, sys_stack_top = 0;
    if (vmm::alloc_stack(AP_STACK_PAGES, AP_GUARD_PAGES,
                         kva::tag::privileged_stack,
                         sys_stack_base, sys_stack_top) != vmm::OK) {
        while (true) { asm volatile("cli; hlt"); }
    }

    // Common AP Init
    if (sched::init_ap(cpu_id, data->stack_top, sys_stack_top) != sched::OK) {
        info->state.store_release(smp::CPU_OFFLINE);
        while (true) { asm volatile("cli; hlt"); }
    }

    if (clock::init_ap() != clock::OK || timer::init_ap(100) != timer::OK) {
        info->state.store_release(smp::CPU_OFFLINE);
        while (true) { asm volatile("cli; hlt"); }
    }

    if (ktrace::init() != ktrace::OK) {
        log::warn("ktrace::init failed on AP %u, performance profiling may be degraded", cpu_id);
    }

    info->state.store_release(smp::CPU_ONLINE);

    sched::run_idle();
}

__PRIVILEGED_CODE static void send_init_ipi(uint32_t apic_id) {
    // Assert INIT
    irq::send_lapic_ipi(apic_id, irq::ICR_DM_INIT | irq::ICR_LEVEL_ASSERT | irq::ICR_TRIGGER_LEVEL);

    // Deassert INIT
    irq::send_lapic_ipi(apic_id, irq::ICR_DM_INIT | irq::ICR_TRIGGER_LEVEL);
}

__PRIVILEGED_CODE static void send_startup_ipi(uint32_t apic_id, uint32_t vector) {
    irq::send_lapic_ipi(apic_id, (vector & 0xFF) | irq::ICR_DM_STARTUP);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t smp_enumerate(smp::cpu_info* cpus, uint32_t max) {
    const acpi::madt_info& madt = acpi::get_madt_info();

    uint32_t bsp_apic_id = 0;
    uint64_t apic_base_msr = msr::read(irq::MSR_IA32_APIC_BASE);
    if (apic_base_msr & irq::APIC_BASE_BSP_FLAG) {
        bsp_apic_id = irq::read_lapic_id();
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < madt.lapic_count && count < max; i++) {
        if (!madt.lapics[i].enabled) {
            continue;
        }

        // Drivers aim MSIs at the CPU that sets them up, so every AP must be reachable
        uint32_t apic_id = madt.lapics[i].apic_id;
        bool is_bsp = (apic_id == bsp_apic_id);
        if (!is_bsp && apic_id > irq::MAX_DEVICE_IRQ_APIC_ID) {
            log::warn("smp: leaving APIC ID %u offline, device interrupts cannot reach it", apic_id);
            continue;
        }

        cpus[count].logical_id = count;
        cpus[count].hw_id = apic_id;
        cpus[count].state.store_relaxed(smp::CPU_OFFLINE);
        cpus[count].is_bsp = is_bsp;
        count++;
    }

    return count;
}

pmm::phys_range smp_fixed_boot_frames() {
    return {AP_TRAMPOLINE_PHYS, AP_STARTUP_DATA_PHYS + pmm::PAGE_SIZE};
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

    paging::flush_tlb_page_local(AP_TRAMPOLINE_PHYS);
    paging::flush_tlb_page_local(AP_STARTUP_DATA_PHYS);

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
    // Allocate per-CPU area (2 pages, zeroed)
    uintptr_t percpu_va = 0;
    int32_t rc = vmm::alloc(PERCPU_PAGES, paging::PAGE_USER_RW,
                            vmm::ALLOC_ZERO, kva::tag::generic, percpu_va);
    if (rc != vmm::OK) {
        log::error("smp: failed to allocate per-CPU area for CPU %u (%d)",
                   cpu.logical_id, rc);
        return smp::ERR_BOOT_TIMEOUT;
    }

    // Allocate task stack
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

    // Fill per-AP fields in startup data
    auto* data = reinterpret_cast<ap_startup_data*>(AP_STARTUP_DATA_PHYS);
    data->stack_top = stack_top;
    data->logical_id = cpu.logical_id;
    data->percpu_base = percpu_va;

    uint32_t apic_id = static_cast<uint32_t>(cpu.hw_id);

    // INIT-SIPI-SIPI sequence
    send_init_ipi(apic_id);
    delay::pit_ms(IPI_INIT_DELAY_MS);

    send_startup_ipi(apic_id, AP_SIPI_VECTOR);
    delay::pit_ms(IPI_SIPI_DELAY_MS);

    // Check if AP came online
    if (cpu.state.load_acquire() == smp::CPU_ONLINE) {
        return smp::OK;
    }

    // Send second SIPI and poll with timeout
    send_startup_ipi(apic_id, AP_SIPI_VECTOR);

    for (uint32_t waited = 0; waited < IPI_TIMEOUT_MS; waited++) {
        delay::pit_ms(1);
        if (cpu.state.load_acquire() == smp::CPU_ONLINE) {
            return smp::OK;
        }
    }

    // A SIPI cannot be recalled, so the deadline expiring does not prove the AP
    // is dead. Abandon atomically: winning means the AP parks before using
    // anything, losing means it is alive and owns its memory.
    uint32_t abandon = smp::CPU_BOOTING;
    if (cpu.state.cmpxchg_strong_acq_rel(abandon, smp::CPU_ABANDONED)) {
        // The trampoline already ran on this stack, so both allocations are
        // leaked rather than handed out again under a CPU that may still run.
        return smp::ERR_BOOT_TIMEOUT;
    }

    // The AP claimed after the deadline, so let it finish coming up.
    for (uint32_t waited = 0; waited < IPI_TIMEOUT_MS &&
         cpu.state.load_acquire() == smp::CPU_CLAIMED; waited++) {
        delay::pit_ms(1);
    }

    return cpu.state.load_acquire() == smp::CPU_ONLINE
        ? smp::OK : smp::ERR_BOOT_TIMEOUT;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_ipi_init() {
    return smp::ipi::OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_ipi_init_ap() {
    return smp::ipi::OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_raise_ipi(const smp::cpu_info& target) {
    uint32_t apic_id = static_cast<uint32_t>(target.hw_id);
    irq::send_lapic_ipi(apic_id, x86::VEC_IPI | irq::ICR_DM_FIXED | irq::ICR_LEVEL_ASSERT);

    return smp::ipi::OK;
}

} // namespace arch
