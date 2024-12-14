#ifdef ARCH_X86_64
#include <arch/x86/pat.h>
#include <arch/x86/msr.h>
#include <arch/x86/cpu_control.h>
#include <interrupts/irq.h>
#include <serial/serial.h>

namespace arch::x86 {
const char* pat_attrib_to_string(pat_attrib_t attrib) {
    switch (attrib.type) {
    case PAT_MEM_TYPE_UC: return "Uncacheable";
    case PAT_MEM_TYPE_WC: return "Write Combining";
    case PAT_MEM_TYPE_WT: return "Write Through";
    case PAT_MEM_TYPE_WP: return "Write Protected";
    case PAT_MEM_TYPE_WB: return "Write Back";
    case PAT_MEM_TYPE_UC_: return "Uncacheable Minus";
    default: break;
    }

    return "Unknown";
}

__PRIVILEGED_CODE
pat_t read_pat_msr() {
    pat_t pat;
    pat.raw = msr::read(IA32_PAT_MSR);

    return pat;
}

__PRIVILEGED_CODE
void write_pat_msr(pat_t pat) {
    msr::write(IA32_PAT_MSR, pat.raw);
}

__PRIVILEGED_CODE
void setup_kernel_pat() {
    uint64_t old_cr0 = 0;

    pat_t pat = read_pat_msr();
    disable_interrupts();

    cpu_cache_disable(&old_cr0);
    cpu_cache_flush();
    cpu_pge_clear();

    pat.pa4.type = PAT_MEM_TYPE_WC;
    pat.pa2.type = PAT_MEM_TYPE_UC;
    write_pat_msr(pat);

    cpu_cache_flush();
    cpu_pge_clear();
    cpu_set_cr0(old_cr0);
    cpu_pge_enable();

    enable_interrupts();
}

__PRIVILEGED_CODE
void debug_kernel_pat() {
    pat_t pat = read_pat_msr();

    serial::printf("---- Page Attribute Table ----\n");
    serial::printf("    pa0: %s\n", pat_attrib_to_string(pat.pa0));
    serial::printf("    pa1: %s\n", pat_attrib_to_string(pat.pa1));
    serial::printf("    pa2: %s\n", pat_attrib_to_string(pat.pa2));
    serial::printf("    pa3: %s\n", pat_attrib_to_string(pat.pa3));
    serial::printf("    pa4: %s\n", pat_attrib_to_string(pat.pa4));
    serial::printf("    pa5: %s\n", pat_attrib_to_string(pat.pa5));
    serial::printf("    pa6: %s\n", pat_attrib_to_string(pat.pa6));
    serial::printf("    pa7: %s\n", pat_attrib_to_string(pat.pa7));
    serial::printf("\n");
}
} // namespace arch::x86

#endif // ARCH_X86_64

