#include "sched/task_exec_core.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "sched/sched_internal.h"
#include "dynpriv/dynpriv.h"
#include "syscall/syscall.h"
#include "trap/trap_frame.h"
#include "defs/exception.h"
#include "percpu/percpu.h"
#include "common/logging.h"

extern "C" char stack_top[];
extern "C" char sys_stack_top[];

DECLARE_PER_CPU(sched::task*, current_task);
DEFINE_PER_CPU(sched::task_exec_core*, current_task_exec);

namespace sched {

static task_exec_core g_boot_exec = {
    .flags = TASK_FLAG_ELEVATED | TASK_FLAG_KERNEL | TASK_FLAG_CAN_ELEVATE
           | TASK_FLAG_IDLE | TASK_FLAG_RUNNING | TASK_FLAG_PREEMPTIBLE,
    .cpu = 0,
    .task_stack_top = 0,
    .system_stack_top = 0,
    .cpu_ctx = {},
};

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_boot_task() {
    g_boot_exec.task_stack_top = reinterpret_cast<uintptr_t>(stack_top);
    g_boot_exec.system_stack_top = reinterpret_cast<uintptr_t>(sys_stack_top);

    this_cpu(current_task_exec) = &g_boot_exec;
    this_cpu(percpu_is_elevated) = (g_boot_exec.flags & TASK_FLAG_ELEVATED) != 0;
    log::info("sched: boot task initialized, flags=0x%x", g_boot_exec.flags);
    return 0;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void save_cpu_context(
    const aarch64::trap_frame* tf, thread_cpu_context* ctx
) {
    for (int i = 0; i < 31; i++) {
        ctx->x[i] = tf->x[i];
    }
    ctx->sp = tf->sp;
    ctx->pc = tf->elr;
    ctx->pstate = tf->spsr;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void load_cpu_context(
    const thread_cpu_context* ctx, aarch64::trap_frame* tf
) {
    for (int i = 0; i < 31; i++) {
        tf->x[i] = ctx->x[i];
    }
    tf->sp = ctx->sp;
    tf->elr = ctx->pc;
    tf->spsr = ctx->pstate;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void arch_init_task_context(
    task* t, void (*entry)(void*), void* arg
) {
    bool elevated = (t->exec.flags & TASK_FLAG_ELEVATED) != 0;
    auto& ctx = t->exec.cpu_ctx;
    ctx.pc = reinterpret_cast<uint64_t>(entry);
    ctx.x[0] = reinterpret_cast<uint64_t>(arg);
    ctx.sp = t->exec.task_stack_top;
    ctx.pstate = elevated ? aarch64::SPSR_EL1T : aarch64::SPSR_EL0T;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void arch_post_switch(task* next) {
    (void)next;
}

void yield() {
    asm volatile("svc %0" : : "i"(syscall::SYS_YIELD) : "memory");
}

/**
 * Called from the aarch64 syscall dispatch when it receives SYS_YIELD.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void on_yield(aarch64::trap_frame* tf) {
    task* prev = current();
    save_cpu_context(tf, &prev->exec.cpu_ctx);

    task* next = pick_next_and_switch(prev);
    if (next == prev) return;

    load_cpu_context(&next->exec.cpu_ctx, tf);
    arch_post_switch(next);
}

/**
 * Called from the aarch64 IRQ handler on timer interrupt (PPI 27).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void on_tick(aarch64::trap_frame* tf) {
    task* prev = current();
    if (!(prev->exec.flags & TASK_FLAG_PREEMPTIBLE)) {
        return;
    }

    save_cpu_context(tf, &prev->exec.cpu_ctx);

    task* next = pick_next_and_switch(prev);
    if (next == prev) {
        return;
    }

    load_cpu_context(&next->exec.cpu_ctx, tf);
    arch_post_switch(next);
}

} // namespace sched
