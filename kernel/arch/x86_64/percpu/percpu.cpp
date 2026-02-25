#include "percpu/percpu.h"
#include "common/types.h"
#include "common/logging.h"
#include "common/string.h"
#include "hw/msr.h"

DEFINE_PER_CPU_BASE(uintptr_t, percpu_offset);

static constexpr uintptr_t CPU0_AREA_SIZE = 0x2000; // 8KB
alignas(0x1000) static uint8_t g_cpu0_area[CPU0_AREA_SIZE];

uintptr_t __per_cpu_offset[MAX_CPUS];

constexpr uint32_t IA32_GS_BASE        = 0xC0000101;
constexpr uint32_t IA32_KERNEL_GS_BASE = 0xC0000102;

namespace percpu {

uintptr_t this_cpu_offset() {
    uintptr_t off;
    asm volatile("mov %%gs:0, %0" : "=r"(off) :: "memory");
    return off;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_bsp() {
    if (size() > CPU0_AREA_SIZE) {
        log::fatal("Per-CPU area too large: %lu > %lu", size(), CPU0_AREA_SIZE);
        return ERR_SIZE_MISMATCH;
    }

    for (uintptr_t i = 0; i < CPU0_AREA_SIZE; i++) {
        g_cpu0_area[i] = 0;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(g_cpu0_area);
    const uintptr_t tmpl = reinterpret_cast<uintptr_t>(__percpu_start);
    const uintptr_t delta = base - tmpl;

    const uintptr_t percpu_off_off =
        reinterpret_cast<uintptr_t>(&percpu_offset) - tmpl;
    if (percpu_off_off != 0) {
        log::fatal("percpu_offset not at offset 0 (got %lu)", percpu_off_off);
        return ERR_LAYOUT;
    }

    *reinterpret_cast<uintptr_t*>(base + percpu_off_off) = delta;

    __per_cpu_offset[0] = delta;

    // Set GS_BASE to kernel percpu
    msr::write(IA32_GS_BASE, base);

    // Set KERNEL_GS_BASE to same value - makes swapgs a no-op.
    // This ensures GS always points to kernel percpu, regardless of
    // whether we're in user/kernel context or elevated state.
    // swapgs + lfence is kept in entry.S as a Spectre mitigation barrier.
    msr::write(IA32_KERNEL_GS_BASE, base);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap(uint32_t cpu_id, uintptr_t base_va) {
    const uintptr_t tmpl = reinterpret_cast<uintptr_t>(__percpu_start);

    const uintptr_t percpu_off_off =
        reinterpret_cast<uintptr_t>(&percpu_offset) - tmpl;
    if (percpu_off_off != 0) {
        return ERR_LAYOUT;
    }

    const uintptr_t delta = base_va - tmpl;

    string::memcpy(reinterpret_cast<void*>(base_va), __percpu_start, size());

    *reinterpret_cast<uintptr_t*>(base_va) = delta;

    __per_cpu_offset[cpu_id] = delta;

    msr::write(IA32_GS_BASE, base_va);
    msr::write(IA32_KERNEL_GS_BASE, base_va);

    this_cpu(percpu_cpu_id) = cpu_id;

    return OK;
}

} // namespace percpu
