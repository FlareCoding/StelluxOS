#ifdef ARCH_X86_64
#include <arch/percpu.h>
#include <arch/x86/msr.h>
#include <memory/paging.h>
#include <memory/vmm.h>

__PRIVILEGED_DATA
__attribute__((aligned(PAGE_SIZE)))
uintptr_t g_per_cpu_area_ptrs[MAX_SYSTEM_CPUS] = { 0 };

char g_bsp_per_cpu_area[PAGE_SIZE] __attribute__((aligned(64)));

namespace arch {
__PRIVILEGED_CODE
void init_bsp_per_cpu_area() {
    g_per_cpu_area_ptrs[BSP_CPU_ID] = (uintptr_t)&g_bsp_per_cpu_area;

#ifdef ARCH_X86_64
    x86::msr::write(IA32_GS_BASE, g_per_cpu_area_ptrs[BSP_CPU_ID]);
    x86::msr::write(IA32_KERNEL_GS_BASE, g_per_cpu_area_ptrs[BSP_CPU_ID]);
#endif // ARCH_X86_64
}

__PRIVILEGED_CODE
void init_ap_per_cpu_area(uint8_t cpu_id) {
#ifdef ARCH_X86_64
    x86::msr::write(IA32_GS_BASE, g_per_cpu_area_ptrs[cpu_id]);
    x86::msr::write(IA32_KERNEL_GS_BASE, g_per_cpu_area_ptrs[cpu_id]);
#endif // ARCH_X86_64
}

__PRIVILEGED_CODE
void allocate_ap_per_cpu_area(uint8_t cpu_id) {
    void* percpu_area = vmm::alloc_virtual_page(DEFAULT_UNPRIV_PAGE_FLAGS | PTE_PCD);
    g_per_cpu_area_ptrs[cpu_id] = reinterpret_cast<uintptr_t>(percpu_area);
}

__PRIVILEGED_CODE
void deallocate_ap_per_cpu_area(uint8_t cpu_id) {
    vmm::unmap_virtual_page(g_per_cpu_area_ptrs[cpu_id]);
}
} // namespace arch

#endif // ARCH_X86_64

