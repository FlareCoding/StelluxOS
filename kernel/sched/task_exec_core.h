#ifndef STELLUX_SCHED_TASK_EXEC_CORE_H
#define STELLUX_SCHED_TASK_EXEC_CORE_H

#include "common/types.h"
#include "percpu/percpu.h"
#include "sched/thread_cpu_context.h"
#include "sched/fpu_state.h"

namespace mm { struct mm_context; }

namespace sched {

// Task privilege-mode bit (Ring 0 / EL1 when set). During trap/syscall handling
// the CPU executes elevated regardless of this bit; per-CPU runtime elevation
// state is tracked separately via percpu_is_elevated.
constexpr uint32_t TASK_FLAG_ELEVATED    = (1 << 0);
constexpr uint32_t TASK_FLAG_KERNEL      = (1 << 1);  // Is a kernel task
constexpr uint32_t TASK_FLAG_CAN_ELEVATE = (1 << 2);  // Authorized to elevate
constexpr uint32_t TASK_FLAG_IDLE        = (1 << 3);  // Is the idle task
constexpr uint32_t TASK_FLAG_IN_SYSCALL  = (1 << 5);  // Currently handling a syscall
constexpr uint32_t TASK_FLAG_IN_IRQ      = (1 << 6);  // Currently in interrupt handler
constexpr uint32_t TASK_FLAG_PREEMPTIBLE = (1 << 7);  // Can be preempted
constexpr uint32_t TASK_FLAG_POSIX_THREAD = (1 << 8); // Created through clone

struct task_exec_core {
    uint32_t  flags;
    uint32_t  cpu;
    uintptr_t task_stack_top;
    uintptr_t system_stack_top;
    thread_cpu_context cpu_ctx;
    uint32_t  on_cpu; // 1 while context is live and executing on a CPU
    uint64_t  pt_root; // physical address of top-level page table (CR3 / TTBR1)
    uint64_t  user_pt_root; // physical address of user-space page table (= pt_root on x86 / TTBR0 on aarch64)
    mm::mm_context* mm_ctx; // owning reference to process address-space metadata
    fpu_state fpu_ctx;
    uint64_t  tls_base; // thread-local storage base (FS_BASE on x86, TPIDR_EL0 on aarch64)

    // Staged full-register return context (x86): a signal
    // frame captured outside a syscall restores through an
    // IRET exit. Consumed by the syscall exit assembly.
    uint32_t  iret_pending;
    thread_cpu_context iret_ctx;
};

constexpr size_t TASK_FLAGS_OFFSET          = __builtin_offsetof(task_exec_core, flags);
constexpr size_t TASK_CPU_OFFSET            = __builtin_offsetof(task_exec_core, cpu);
constexpr size_t TASK_STACK_OFFSET          = __builtin_offsetof(task_exec_core, task_stack_top);
constexpr size_t TASK_SYS_STACK_OFFSET      = __builtin_offsetof(task_exec_core, system_stack_top);
constexpr size_t TASK_CPU_CTX_OFFSET        = __builtin_offsetof(task_exec_core, cpu_ctx);
constexpr size_t TASK_PT_ROOT_OFFSET        = __builtin_offsetof(task_exec_core, pt_root);
constexpr size_t TASK_USER_PT_ROOT_OFFSET   = __builtin_offsetof(task_exec_core, user_pt_root);
constexpr size_t TASK_MM_CTX_OFFSET         = __builtin_offsetof(task_exec_core, mm_ctx);
constexpr size_t TASK_FPU_CTX_OFFSET        = __builtin_offsetof(task_exec_core, fpu_ctx);
constexpr size_t TASK_IRET_PENDING_OFFSET   = __builtin_offsetof(task_exec_core, iret_pending);
constexpr size_t TASK_IRET_CTX_OFFSET       = __builtin_offsetof(task_exec_core, iret_ctx);

// Static assertions to ensure assembly offsets remain in sync
// If these fail, update the assembly constants in:
//   - kernel/arch/x86_64/trap/entry.S (TASK_FLAGS_OFFSET, TASK_SYS_STACK_OFFSET)
//   - kernel/arch/x86_64/syscall/syscall_entry.S (TASK_FLAGS_OFFSET, TASK_SYS_STACK_OFFSET,
//     TASK_IRET_PENDING_OFFSET, TASK_IRET_CTX_OFFSET, CTX_* register slots)
static_assert(TASK_FLAGS_OFFSET == 0x00, "TASK_FLAGS_OFFSET changed - update x86_64 entry.S and syscall_entry.S");
static_assert(TASK_SYS_STACK_OFFSET == 0x10, "TASK_SYS_STACK_OFFSET changed - update x86_64 entry.S and syscall_entry.S");

#if defined(__x86_64__)
static_assert(TASK_IRET_PENDING_OFFSET == 0x2E8, "TASK_IRET_PENDING_OFFSET changed - update syscall_entry.S");
static_assert(TASK_IRET_CTX_OFFSET == 0x2F0, "TASK_IRET_CTX_OFFSET changed - update syscall_entry.S");
static_assert(__builtin_offsetof(thread_cpu_context, rax) == 0x00);
static_assert(__builtin_offsetof(thread_cpu_context, rcx) == 0x10);
static_assert(__builtin_offsetof(thread_cpu_context, rsp) == 0x38);
static_assert(__builtin_offsetof(thread_cpu_context, r11) == 0x58);
static_assert(__builtin_offsetof(thread_cpu_context, rip) == 0x80);
static_assert(__builtin_offsetof(thread_cpu_context, rflags) == 0x88);
static_assert(__builtin_offsetof(thread_cpu_context, cs) == 0x90);
static_assert(__builtin_offsetof(thread_cpu_context, ss) == 0x98);
#endif

int32_t init_boot_task();

} // namespace sched

DECLARE_PER_CPU(sched::task_exec_core*, current_task_exec);

#endif // STELLUX_SCHED_TASK_EXEC_CORE_H
