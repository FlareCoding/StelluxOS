#include "sched/task_exec_core.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "sched/sched_internal.h"
#include "dynpriv/dynpriv.h"
#include "trap/trap_frame.h"
#include "defs/segments.h"
#include "defs/vectors.h"
#include "percpu/percpu.h"
#include "gdt/gdt.h"
#include "common/logging.h"

extern "C" char stack_top[];

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
    g_boot_exec.system_stack_top = x86::gdt::get_bsp_kernel_stack_top();

    this_cpu(current_task_exec) = &g_boot_exec;
    this_cpu(percpu_is_elevated) = (g_boot_exec.flags & TASK_FLAG_ELEVATED) != 0;
    log::info("sched: boot task initialized, flags=0x%x", g_boot_exec.flags);
    return 0;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void save_cpu_context(
    const x86::trap_frame* tf, thread_cpu_context* ctx
) {
    ctx->rax = tf->rax; ctx->rbx = tf->rbx; ctx->rcx = tf->rcx; ctx->rdx = tf->rdx;
    ctx->rsi = tf->rsi; ctx->rdi = tf->rdi; ctx->rbp = tf->rbp; ctx->rsp = tf->rsp;
    ctx->r8  = tf->r8;  ctx->r9  = tf->r9;  ctx->r10 = tf->r10; ctx->r11 = tf->r11;
    ctx->r12 = tf->r12; ctx->r13 = tf->r13; ctx->r14 = tf->r14; ctx->r15 = tf->r15;
    ctx->rip = tf->rip;
    ctx->rflags = tf->rflags;
    ctx->cs = tf->cs;
    ctx->ss = tf->ss;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void load_cpu_context(
    const thread_cpu_context* ctx, x86::trap_frame* tf
) {
    tf->rax = ctx->rax; tf->rbx = ctx->rbx; tf->rcx = ctx->rcx; tf->rdx = ctx->rdx;
    tf->rsi = ctx->rsi; tf->rdi = ctx->rdi; tf->rbp = ctx->rbp; tf->rsp = ctx->rsp;
    tf->r8  = ctx->r8;  tf->r9  = ctx->r9;  tf->r10 = ctx->r10; tf->r11 = ctx->r11;
    tf->r12 = ctx->r12; tf->r13 = ctx->r13; tf->r14 = ctx->r14; tf->r15 = ctx->r15;
    tf->rip = ctx->rip;
    tf->rflags = ctx->rflags;
    tf->cs = ctx->cs;
    tf->ss = ctx->ss;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void arch_init_task_context(
    task* t, void (*entry)(void*), void* arg
) {
    bool elevated = (t->exec.flags & TASK_FLAG_ELEVATED) != 0;
    auto& ctx = t->exec.cpu_ctx;
    ctx.rip = reinterpret_cast<uint64_t>(entry);
    ctx.rdi = reinterpret_cast<uint64_t>(arg);
    ctx.rsp = t->exec.task_stack_top;
    ctx.rflags = 0x202; // IF=1, reserved bit 1=1
    ctx.cs = elevated ? x86::KERNEL_CS : x86::USER_CS;
    ctx.ss = elevated ? x86::KERNEL_DS : x86::USER_DS;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void arch_post_switch(task* next) {
    this_cpu(current_task_exec) = &next->exec;
    this_cpu(percpu_is_elevated) = (next->exec.flags & TASK_FLAG_ELEVATED) != 0;

    // Update TSS.RSP0 so Ring 3 -> Ring 0 transitions use the new task's system stack
    if (next->exec.system_stack_top) {
        x86::gdt::set_rsp0(next->exec.system_stack_top);
    }
}

void yield() {
    asm volatile("int %0" : : "i"(x86::VEC_SCHED_YIELD) : "memory");
}

/**
 * Called from the x86_64 trap handler when it receives VEC_SCHED_YIELD.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void on_yield(x86::trap_frame* tf) {
    task* prev = current();
    save_cpu_context(tf, &prev->exec.cpu_ctx);

    task* next = pick_next_and_switch(prev);
    if (next == prev) return;

    load_cpu_context(&next->exec.cpu_ctx, tf);
    arch_post_switch(next);
}

/**
 * Called from the x86_64 trap handler on timer interrupt (VEC_TIMER).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void on_tick(x86::trap_frame* tf) {
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
