#ifdef ARCH_X86_64
#include <arch/percpu.h>
#include <arch/x86/msr.h>
#include <memory/paging.h>

__PRIVILEGED_DATA
__attribute__((aligned(PAGE_SIZE)))
uintptr_t g_per_cpu_area_ptrs[MAX_SYSTEM_CPUS] = { 0 };

char g_bsp_per_cpu_area[PAGE_SIZE] __attribute__((aligned(64)));

namespace arch {
__PRIVILEGED_CODE
void init_bsp_per_cpu_area() {
#ifdef ARCH_X86_64
    g_per_cpu_area_ptrs[BSP_CPU_ID] = (uintptr_t)&g_bsp_per_cpu_area;

    x86::msr::write(IA32_GS_BASE, g_per_cpu_area_ptrs[BSP_CPU_ID]);
    x86::msr::write(IA32_KERNEL_GS_BASE, g_per_cpu_area_ptrs[BSP_CPU_ID]);
#endif // ARCH_X86_64
}
} // namespace arch

#endif // ARCH_X86_64

