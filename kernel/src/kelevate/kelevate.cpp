#include "kelevate.h"
#include <syscall/syscalls.h>
#include <arch/x86/gsfsbase.h>
#include <kprint.h>

EXTERN_C int __check_current_elevate_status() {
    return static_cast<int>(current->elevated);
}

// Has to be called from usercode's context right after __kelevate()
__PRIVILEGED_CODE
void __set_elevated_usergs() {
    current->usergs = rdgsbase();
    
    // Current gsbase is set to user gsbase
    swapgs();
    uint64_t kgsbase = rdgsbase();
    swapgs();

    // Set user gsbase to be the same as kernel gsbase
    wrgsbase(kgsbase);
}

__PRIVILEGED_CODE
void __restore_lowered_usergs() {
    wrgsbase(current->usergs);
}

EXTERN_C void __call_lowered_entry_asm(void* entry, void* stack, uint64_t flags);

void __kelevate() {
    __syscall(SYSCALL_SYS_ELEVATE, 0, 0, 0, 0, 0, 0);
    __set_elevated_usergs();
}

void __klower() {
    __restore_lowered_usergs();

    __asm__ __volatile__ (
        "pushfq;"            // Push EFLAGS onto the stack
        "popq %%r11;"        // Pop EFLAGS into r11 (as required by SYSRET)
        "cli;"               // Disable interrupts
        "lea 1f(%%rip), %%rcx;"  // Load the address of the next instruction into rcx
        "movq %%gs:0x0, %%rax;" // Move the address of current task into rax
        "btrq $0, 0xf8(%%rax);"  // Set current->elevated to 0
        "sysretq;"            // Execute SYSRET and IF flag will get reset from the eflags
        "1:"                 // Label for the next instruction after SYSRET
        : /* no outputs */
        : /* no inputs */
        : "rcx", "r11", "memory"   // Clobbered registers
    );
}

long __kcheck_elevated() {
    return __syscall(SYSCALL_SYS_ELEVATE, 1, 0, 0, 0, 0, 0);
}

void __call_lowered_entry(lowered_entry_fn_t entry, void* user_stack) {
    __call_lowered_entry_asm((void*)entry, (void*)((uint64_t)user_stack), 0x200);
}
