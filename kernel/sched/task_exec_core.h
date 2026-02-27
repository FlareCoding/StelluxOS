#ifndef STELLUX_SCHED_TASK_EXEC_CORE_H
#define STELLUX_SCHED_TASK_EXEC_CORE_H

#include "common/types.h"
#include "percpu/percpu.h"
#include "sched/thread_cpu_context.h"
#include "sched/fpu_state.h"

namespace sched {

constexpr uint32_t TASK_FLAG_ELEVATED    = (1 << 0);  // Currently at ring 0 / EL1
constexpr uint32_t TASK_FLAG_KERNEL      = (1 << 1);  // Is a kernel task
constexpr uint32_t TASK_FLAG_CAN_ELEVATE = (1 << 2);  // Authorized to elevate
constexpr uint32_t TASK_FLAG_IDLE        = (1 << 3);  // Is the idle task
constexpr uint32_t TASK_FLAG_IN_SYSCALL  = (1 << 5);  // Currently handling a syscall
constexpr uint32_t TASK_FLAG_IN_IRQ      = (1 << 6);  // Currently in interrupt handler
constexpr uint32_t TASK_FLAG_PREEMPTIBLE = (1 << 7);  // Can be preempted

struct task_exec_core {
    uint32_t  flags;
    uint32_t  cpu;
    uintptr_t task_stack_top;
    uintptr_t system_stack_top;
    thread_cpu_context cpu_ctx;
    uint32_t  on_cpu; // 1 while context is live and executing on a CPU
    uint64_t  pt_root; // physical address of top-level page table
    fpu_state fpu_ctx;
};

constexpr size_t TASK_FLAGS_OFFSET     = __builtin_offsetof(task_exec_core, flags);
constexpr size_t TASK_CPU_OFFSET       = __builtin_offsetof(task_exec_core, cpu);
constexpr size_t TASK_STACK_OFFSET     = __builtin_offsetof(task_exec_core, task_stack_top);
constexpr size_t TASK_SYS_STACK_OFFSET = __builtin_offsetof(task_exec_core, system_stack_top);
constexpr size_t TASK_CPU_CTX_OFFSET   = __builtin_offsetof(task_exec_core, cpu_ctx);
constexpr size_t TASK_PT_ROOT_OFFSET   = __builtin_offsetof(task_exec_core, pt_root);
constexpr size_t TASK_FPU_CTX_OFFSET   = __builtin_offsetof(task_exec_core, fpu_ctx);

// Static assertions to ensure assembly offsets remain in sync
// If these fail, update the assembly constants in:
//   - kernel/arch/x86_64/trap/entry.S (TASK_FLAGS_OFFSET, TASK_SYS_STACK_OFFSET)
static_assert(TASK_FLAGS_OFFSET == 0x00, "TASK_FLAGS_OFFSET changed - update x86_64 entry.S");
static_assert(TASK_SYS_STACK_OFFSET == 0x10, "TASK_SYS_STACK_OFFSET changed - update x86_64 entry.S");

int32_t init_boot_task();

} // namespace sched

DECLARE_PER_CPU(sched::task_exec_core*, current_task_exec);

#endif // STELLUX_SCHED_TASK_EXEC_CORE_H
