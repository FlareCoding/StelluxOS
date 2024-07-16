#include "pat.h"
#include <kstring.h>
#include <kprint.h>

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

void debugPat(pat_t pat) {
    kuPrint("---- Page Attribute Table ----\n");
    kuPrint("    pa0: %s\n", patAttribToString(pat.pa0).c_str());
    kuPrint("    pa1: %s\n", patAttribToString(pat.pa1).c_str());
    kuPrint("    pa2: %s\n", patAttribToString(pat.pa2).c_str());
    kuPrint("    pa3: %s\n", patAttribToString(pat.pa3).c_str());
    kuPrint("    pa4: %s\n", patAttribToString(pat.pa4).c_str());
    kuPrint("    pa5: %s\n", patAttribToString(pat.pa5).c_str());
    kuPrint("    pa6: %s\n", patAttribToString(pat.pa6).c_str());
    kuPrint("    pa7: %s\n", patAttribToString(pat.pa7).c_str());
    kuPrint("\n");
}

