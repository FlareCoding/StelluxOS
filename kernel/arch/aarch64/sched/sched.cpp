#include "sched/task_exec_core.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "sched/sched_internal.h"
#include "sched/fpu.h"
#include "dynpriv/dynpriv.h"
#include "syscall/syscall.h"
#include "trap/trap_frame.h"
#include "defs/exception.h"
#include "percpu/percpu.h"
#include "hw/cpu.h"
#include "mm/paging.h"
#include "mm/paging_arch.h"
#include "common/logging.h"

extern "C" char stack_top[];
extern "C" char sys_stack_top[];

DECLARE_PER_CPU(sched::task*, current_task);
DEFINE_PER_CPU(sched::task_exec_core*, current_task_exec);

namespace sched {

static task_exec_core g_boot_exec = {
    .flags = TASK_FLAG_ELEVATED | TASK_FLAG_KERNEL | TASK_FLAG_CAN_ELEVATE
           | TASK_FLAG_IDLE | TASK_FLAG_PREEMPTIBLE,
    .cpu = 0,
    .task_stack_top = 0,
    .system_stack_top = 0,
    .cpu_ctx = {},
    .on_cpu = 0,
    .pt_root = 0,
    .user_pt_root = 0,
    .mm_ctx = nullptr,
    .fpu_ctx = {},
    .tls_base = 0,
};

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_boot_task() {
    g_boot_exec.task_stack_top = reinterpret_cast<uintptr_t>(stack_top);
    g_boot_exec.system_stack_top = reinterpret_cast<uintptr_t>(sys_stack_top);
    g_boot_exec.pt_root = paging::get_kernel_pt_root();
    g_boot_exec.mm_ctx = nullptr;
    g_boot_exec.on_cpu = 1;

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
    // ctx->sp tracks the interrupted stack pointer for the return mode in ctx->pstate:
    // - EL0t / EL1t: SP_EL0
    // - EL1h:        SP_EL1
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
__PRIVILEGED_CODE static void prepare_trap_return_stacks(
    aarch64::trap_frame* tf, const task* next
) {
    // trap_frame.sp is consumed as the return stack for the selected mode:
    // - EL0t / EL1t -> SP_EL0
    // - EL1h        -> SP_EL1
    //
    // trap_frame.sp_el1 is consumed by vector epilogue when returning to
    // EL0t/EL1t, establishing the selected task's EL1 exception stack for
    // subsequent traps.
    tf->sp_el1 = next->exec.system_stack_top;

#ifdef DEBUG
    uint64_t mode = tf->spsr & aarch64::SPSR_MODE_MASK;
    if (mode != aarch64::SPSR_EL1H && tf->sp_el1 == 0) {
        log::fatal("sched/aarch64: missing system_stack_top for non-EL1h return");
    }
#endif
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
    if (paging::get_kernel_pt_root() != next->exec.pt_root) {
        paging::set_kernel_pt_root(next->exec.pt_root);
        paging::flush_tlb_all();
    }
    paging::write_ttbr0_el1(next->exec.user_pt_root);
}

void yield() {
    asm volatile(
        "mov x8, %0\n\t"
        "svc #0"
        :
        : "r"(static_cast<uint64_t>(syscall::SYS_YIELD))
        : "x8", "memory"
    );
}

/**
 * Called from the aarch64 syscall dispatch when it receives SYS_YIELD.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void on_yield(aarch64::trap_frame* tf) {
    task* prev = current();
    // Advance per-CPU sync epoch so stack reclaim can wait for a post-switch TLB-safe point.
    advance_cpu_tlb_sync_epoch();
    // Publish prior switched-out task as off-CPU before we start a new scheduling decision.
    finalize_pending_off_cpu();
    save_cpu_context(tf, &prev->exec.cpu_ctx);
    prev->exec.tls_base = cpu::read_tls_base();

    task* next = pick_next_and_switch(prev);
    if (next == prev) return;

    next->exec.cpu = percpu::current_cpu_id();
    __atomic_store_n(&next->exec.on_cpu, 1, __ATOMIC_RELAXED);

    fpu::save(&prev->exec.fpu_ctx);
    fpu::restore(&next->exec.fpu_ctx);

    load_cpu_context(&next->exec.cpu_ctx, tf);
    prepare_trap_return_stacks(tf, next);
    cpu::write_tls_base(next->exec.tls_base);
    arch_post_switch(next);
    // Defer prev->on_cpu clear until switch teardown is complete.
    defer_off_cpu_finalize(prev);
}

/**
 * Called from the aarch64 IRQ handler on timer interrupt (PPI 27).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void on_tick(aarch64::trap_frame* tf) {
    task* prev = current();
    // Each scheduler trap is a synchronization checkpoint for deferred reclaim logic.
    advance_cpu_tlb_sync_epoch();
    // Finish prior off-CPU publication before handling this tick's switch.
    finalize_pending_off_cpu();
    if (!(prev->exec.flags & TASK_FLAG_PREEMPTIBLE)) {
        return;
    }

    save_cpu_context(tf, &prev->exec.cpu_ctx);
    prev->exec.tls_base = cpu::read_tls_base();

    task* next = pick_next_and_switch(prev);
    if (next == prev) {
        return;
    }

    next->exec.cpu = percpu::current_cpu_id();
    __atomic_store_n(&next->exec.on_cpu, 1, __ATOMIC_RELAXED);

    fpu::save(&prev->exec.fpu_ctx);
    fpu::restore(&next->exec.fpu_ctx);

    load_cpu_context(&next->exec.cpu_ctx, tf);
    prepare_trap_return_stacks(tf, next);
    cpu::write_tls_base(next->exec.tls_base);
    arch_post_switch(next);
    // Prevent early off-CPU publication while trap exit still depends on prev context.
    defer_off_cpu_finalize(prev);
}

} // namespace sched
