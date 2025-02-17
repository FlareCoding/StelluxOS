#include <dynpriv/dynpriv.h>
#include <process/process.h>
#include <syscall/syscalls.h>
#include <kstl/hashmap.h>

EXTERN_C int __check_current_elevate_status() {
    return static_cast<int>(current->elevated);
}

namespace dynpriv {
__PRIVILEGED_DATA
uint64_t g_dynpriv_blessed_asid = 0;

__PRIVILEGED_DATA
kstl::hashmap<uint64_t, bool>* g_dynpriv_whitelisted_asids = nullptr;

__PRIVILEGED_CODE
void set_blessed_kernel_asid() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    g_dynpriv_blessed_asid = cr3;
}

__PRIVILEGED_CODE
bool is_asid_allowed() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    // First check if the ASID is a blessed kernel
    // ASID, which is automatically allowed.
    if (g_dynpriv_blessed_asid == cr3) {
        return true;
    }

    // Check if the ASID whitelisted has not been initialized
    if (!g_dynpriv_whitelisted_asids) {
        return false;
    }

    // ASID is not whitelisted
    if (!g_dynpriv_whitelisted_asids->find(cr3)) {
        return false;
    }

    return (*g_dynpriv_whitelisted_asids)[cr3];
}

__PRIVILEGED_CODE
void initialize_dynpriv_asid_whitelist() {
    // If it's already initialized, no need to do it again
    if (g_dynpriv_whitelisted_asids) {
        return;
    }

    g_dynpriv_whitelisted_asids = new kstl::hashmap<uint64_t, bool>();
}

__PRIVILEGED_CODE
void whitelist_asid(uintptr_t asid) {
    // Ensure that the whitelist map exists
    if (!g_dynpriv_whitelisted_asids) {
        return;
    }

    auto& whitelist = *g_dynpriv_whitelisted_asids;
    whitelist[asid] = true;
}

__PRIVILEGED_CODE
void blacklist_asid(uintptr_t asid) {
    // Ensure that the whitelist map exists
    if (!g_dynpriv_whitelisted_asids) {
        return;
    }

    if (!g_dynpriv_whitelisted_asids->find(asid)) {
        return;
    }

    auto& whitelist = *g_dynpriv_whitelisted_asids;
    whitelist.remove(asid);
}

void elevate() {
    syscall(SYSCALL_SYS_ELEVATE, 0, 0, 0, 0, 0, 0);
}

void lower() {
    asm volatile(
        "pushfq;"                // Push EFLAGS onto the stack
        "popq %%r11;"            // Pop EFLAGS into r11 (as required by SYSRET)
        "cli;"                   // Disable interrupts
        "lea 1f(%%rip), %%rcx;"  // Load the address of the next instruction into rcx
        "btrq $0, 0x100(%0);"    // Set current->elevated to 0
        "sysretq;"               // Execute SYSRET and IF flag will get reset from the eflags
        "1:"                     // Label for the next instruction after SYSRET
        : /* no outputs */
        : "r"(current)
        : "rcx", "r11", "memory"   // Clobbered registers
    );
}

void lower(void* target_fn) {
    asm volatile(
        "pushfq;"                            // Push RFLAGS onto the stack
        "popq %%r11;"                        // Pop RFLAGS into R11 (required by SYSRETQ)
        "cli;"                               // Disable interrupts
        "mov %0, %%rcx;"                     // Move target_fn into RCX
        "btrq $0, 0x100(%0);"                // Set current->elevated to 0
        "sysretq;"                           // Execute SYSRETQ to return to target_fn
        :
        : "r"(current),                      // %0 = pointer to current TCB
          "r"(target_fn)                     // %1 = the target function
        : "rcx", "r11", "rax", "memory"      // Clobbered registers
    );
}

bool is_elevated() {
    return static_cast<bool>(__check_current_elevate_status());
}
} // namespace dynpriv
