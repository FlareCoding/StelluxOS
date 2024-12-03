#include <dynpriv/dynpriv.h>
#include <process/process.h>
#include <syscall/syscalls.h>

EXTERN_C int __check_current_elevate_status() {
    return static_cast<int>(current->elevated);
}

namespace dynpriv {
__PRIVILEGED_DATA
uint64_t g_dynpriv_blessed_asid = 0;

__PRIVILEGED_CODE
void use_current_asid() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    g_dynpriv_blessed_asid = cr3;
}

__PRIVILEGED_CODE
bool is_asid_allowed() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    return g_dynpriv_blessed_asid == cr3;
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
        "movq %%gs:0x0, %%rax;"  // Move the address of current task into rax
        "btrq $0, 0xf0(%%rax);"  // Set current->elevated to 0
        "sysretq;"               // Execute SYSRET and IF flag will get reset from the eflags
        "1:"                     // Label for the next instruction after SYSRET
        : /* no outputs */
        : /* no inputs */
        : "rcx", "r11", "memory"   // Clobbered registers
    );
}

void lower(void* target_fn) {
    asm volatile(
        "pushfq;"                            // Push RFLAGS onto the stack
        "popq %%r11;"                        // Pop RFLAGS into R11 (required by SYSRETQ)
        "cli;"                               // Disable interrupts
        "mov %0, %%rcx;"                     // Move target_fn into RCX
        "movq %%gs:0x0, %%rax;"              // Move the address of current task into rax
        "btrq $0, 0xf0(%%rax);"              // Set current->elevated to 0
        "sysretq;"                           // Execute SYSRETQ to return to target_fn
        :
        : "r" (target_fn)                    // Input operand: target_fn in any general-purpose register
        : "rcx", "r11", "rax", "memory"      // Clobbered registers
    );
}

bool is_elevated() {
    return static_cast<bool>(__check_current_elevate_status());
}
} // namespace dynpriv
