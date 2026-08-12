#ifndef STELLUX_ARCH_X86_64_SYSCALL_SYSCALL_FRAME_H
#define STELLUX_ARCH_X86_64_SYSCALL_SYSCALL_FRAME_H

#include "sched/task_exec_core.h"

namespace x86 {

// User state saved by stlx_x86_syscall_entry at the top of the system
// stack. Field order mirrors the push sequence in syscall_entry.S.
struct syscall_frame {
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t rflags; // user RFLAGS, carried in R11 by SYSCALL/SYSRET
    uint64_t rip;    // user return RIP, carried in RCX by SYSCALL/SYSRET
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r15;
    uint64_t r14;
    uint64_t rsp;    // user stack pointer from the handoff record
};

// Field offset checks matching the syscall_entry.S push sequence
static_assert(__builtin_offsetof(syscall_frame, rdi) == 0x00);
static_assert(__builtin_offsetof(syscall_frame, rsi) == 0x08);
static_assert(__builtin_offsetof(syscall_frame, rdx) == 0x10);
static_assert(__builtin_offsetof(syscall_frame, r10) == 0x18);
static_assert(__builtin_offsetof(syscall_frame, r8) == 0x20);
static_assert(__builtin_offsetof(syscall_frame, r9) == 0x28);
static_assert(__builtin_offsetof(syscall_frame, rflags) == 0x30);
static_assert(__builtin_offsetof(syscall_frame, rip) == 0x38);
static_assert(__builtin_offsetof(syscall_frame, rbx) == 0x40);
static_assert(__builtin_offsetof(syscall_frame, rbp) == 0x48);
static_assert(__builtin_offsetof(syscall_frame, r12) == 0x50);
static_assert(__builtin_offsetof(syscall_frame, r13) == 0x58);
static_assert(__builtin_offsetof(syscall_frame, r15) == 0x60);
static_assert(__builtin_offsetof(syscall_frame, r14) == 0x68);
static_assert(__builtin_offsetof(syscall_frame, rsp) == 0x70);

static_assert(sizeof(syscall_frame) == 0x78);

/**
 * @brief The current task's saved syscall frame.
 * Valid only while a syscall is being handled, when the frame sits
 * immediately below the task's system stack top.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline syscall_frame* current_syscall_frame() {
    return reinterpret_cast<syscall_frame*>(
        this_cpu(current_task_exec)->system_stack_top - sizeof(syscall_frame));
}

} // namespace x86

#endif // STELLUX_ARCH_X86_64_SYSCALL_SYSCALL_FRAME_H
