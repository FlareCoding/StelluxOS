#include "pat.h"
#include <kstring.h>
#include <kprint.h>
#include <interrupts/interrupts.h>
#include "msr.h"
#include "x86_cpu_control.h"

kstl::string patAttribToString(pat_attrib_t attrib) {
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
pat_t readPatMsr() {
    pat_t pat;
    pat.raw = readMsr(IA32_PAT_MSR);

    return pat;
}

__PRIVILEGED_CODE
void writePatMsr(pat_t pat) {
    writeMsr(IA32_PAT_MSR, pat.raw);
}

__PRIVILEGED_CODE
void ksetupPatOnKernelEntry() {
    uint64_t old_cr0 = 0;

    pat_t pat = readPatMsr();
    disableInterrupts();

    x86_cpu_cache_disable(&old_cr0);
    x86_cpu_cache_flush();
    x86_cpu_pge_clear();

    pat.pa4.type = PAT_MEM_TYPE_WC;
    pat.pa2.type = PAT_MEM_TYPE_UC;
    writePatMsr(pat);

    x86_cpu_cache_flush();
    x86_cpu_pge_clear();
    x86_cpu_set_cr0(old_cr0);
    x86_cpu_pge_enable();

    enableInterrupts();
}

void debugPat(pat_t pat) {
    kprintf("---- Page Attribute Table ----\n");
    kprintf("    pa0: %s\n", patAttribToString(pat.pa0).c_str());
    kprintf("    pa1: %s\n", patAttribToString(pat.pa1).c_str());
    kprintf("    pa2: %s\n", patAttribToString(pat.pa2).c_str());
    kprintf("    pa3: %s\n", patAttribToString(pat.pa3).c_str());
    kprintf("    pa4: %s\n", patAttribToString(pat.pa4).c_str());
    kprintf("    pa5: %s\n", patAttribToString(pat.pa5).c_str());
    kprintf("    pa6: %s\n", patAttribToString(pat.pa6).c_str());
    kprintf("    pa7: %s\n", patAttribToString(pat.pa7).c_str());
    kprintf("\n");
}

