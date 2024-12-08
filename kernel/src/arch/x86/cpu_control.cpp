#ifdef ARCH_X86_64
#include <arch/x86/cpu_control.h>

namespace arch::x86 {
__PRIVILEGED_CODE
uint64_t __read_cr0() {
    uint64_t cr0;
    asm volatile(
        "mov %%cr0, %0"
        : "=r" (cr0)
    );
    return cr0;
}

__PRIVILEGED_CODE
void __write_cr0(uint64_t cr0) {
    asm volatile(
        "mov %0, %%cr0"
        :
        : "r" (cr0)
    );
}

__PRIVILEGED_CODE
uint64_t __read_cr4() {
    uint64_t cr4;
    __asm__ __volatile__ (
        "mov %%cr4, %0"
        : "=r" (cr4)
    );
    return cr4;
}

__PRIVILEGED_CODE
void __write_cr4(uint64_t cr4) {
    __asm__ __volatile__ (
        "mov %0, %%cr4"
        :
        : "r" (cr4)
    );
}

__PRIVILEGED_CODE
void cpu_cache_disable(uint64_t* old_cr0) {
    uint64_t cr0 = __read_cr0();
    *old_cr0 = cr0;

    cr0 |= CR0_CD;

    /*
    * Clear the NW (not write-through) bit in order to set caching
    * in "No-fill Cache Mode", where the memory coherency is maintained.
    */
    cr0 &= ~CR0_NW;

    __write_cr0(cr0);
}

__PRIVILEGED_CODE
void cpu_set_cr0(uint64_t cr0) {
    __write_cr0(cr0);
}

__PRIVILEGED_CODE
void cpu_cache_flush() {
    asm volatile("wbinvd");
}

__PRIVILEGED_CODE
void cpu_pge_clear() {
    uint64_t cr4 = __read_cr4();

    // Clear the Page Global Enabled (PGE) bit
    cr4 &= ~CR4_PGE;

    __write_cr4(cr4);
}

__PRIVILEGED_CODE
void cpu_pge_enable() {
    uint64_t cr4 = __read_cr4();

    // Set the Page Global Enabled (PGE) bit
    cr4 |= CR4_PGE;

    __write_cr4(cr4);
}
} // namespace arch::x86

#endif // ARCH_X86_64
