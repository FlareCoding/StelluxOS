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
#include "mm/vma.h"
#include "common/logging.h"

extern "C" char stack_top[];
extern "C" char sys_stack_top[];

DECLARE_PER_CPU(sched::task*, current_task);
DEFINE_PER_CPU(sched::task_exec_core*, current_task_exec);

namespace sched {

__PRIVILEGED_DATA static bool g_logged_first_user_switch = false;
constexpr uint64_t DESC_ADDR_MASK_4K = 0x0000FFFFFFFFF000ULL;

__PRIVILEGED_CODE static void log_user_walk_raw(
    uint64_t root_pt, uintptr_t va, const char* tag
) {
    auto parts = paging::split_virt_addr(va);
    auto* l0 = static_cast<paging::translation_table_t*>(paging::phys_to_virt(root_pt));
    uint64_t l0e = l0->raw[parts.l0_idx];
    log::info("sched[a64]: %s walk L0[%u]=0x%016lx", tag, static_cast<uint32_t>(parts.l0_idx), l0e);
    if ((l0e & 1ULL) == 0 || (l0e & 2ULL) == 0) {
        return;
    }

    uint64_t l1_phys = l0e & DESC_ADDR_MASK_4K;
    auto* l1 = static_cast<paging::translation_table_t*>(paging::phys_to_virt(l1_phys));
    uint64_t l1e = l1->raw[parts.l1_idx];
    log::info("sched[a64]: %s walk L1[%u]=0x%016lx", tag, static_cast<uint32_t>(parts.l1_idx), l1e);
    if ((l1e & 1ULL) == 0) {
        return;
    }
    if ((l1e & 2ULL) == 0) {
        log::info("sched[a64]: %s walk hit 1GB block at L1", tag);
        return;
    }

    uint64_t l2_phys = l1e & DESC_ADDR_MASK_4K;
    auto* l2 = static_cast<paging::translation_table_t*>(paging::phys_to_virt(l2_phys));
    uint64_t l2e = l2->raw[parts.l2_idx];
    log::info("sched[a64]: %s walk L2[%u]=0x%016lx", tag, static_cast<uint32_t>(parts.l2_idx), l2e);
    if ((l2e & 1ULL) == 0) {
        return;
    }
    if ((l2e & 2ULL) == 0) {
        log::info("sched[a64]: %s walk hit 2MB block at L2", tag);
        return;
    }

    uint64_t l3_phys = l2e & DESC_ADDR_MASK_4K;
    auto* l3 = static_cast<paging::translation_table_t*>(paging::phys_to_virt(l3_phys));
    uint64_t l3e = l3->raw[parts.l3_idx];
    log::info("sched[a64]: %s walk L3[%u]=0x%016lx", tag, static_cast<uint32_t>(parts.l3_idx), l3e);
}

/**
 * Emit one-shot diagnostics for the first EL0 switch so we can confirm
 * whether configured user VA addresses fit the active TTBR0 translation size.
 */
__PRIVILEGED_CODE static void log_first_user_switch_diagnostics(const task* next) {
    if (!next || g_logged_first_user_switch) {
        return;
    }
    if (next->exec.user_pt_root == 0 || (next->exec.flags & TASK_FLAG_ELEVATED)) {
        return;
    }
    g_logged_first_user_switch = true;

    uint64_t tcr = paging::read_tcr_el1();
    uint64_t t0sz = tcr & 0x3FULL;
    uint64_t t1sz = (tcr >> 16) & 0x3FULL;
    uint64_t ttbr0 = paging::read_ttbr0_el1();
    uint64_t ttbr1 = paging::read_ttbr1_el1();
    uint64_t va_bits_ttbr0 = 64 - t0sz;
    uint64_t va_bits_ttbr1 = 64 - t1sz;

    uintptr_t stack_probe = mm::USER_STACK_TOP - 0x80;
    bool probe_mapped = paging::is_mapped(stack_probe, next->exec.user_pt_root);
    uint64_t probe_phys = paging::get_physical(stack_probe, next->exec.user_pt_root);
    auto parts = paging::split_virt_addr(stack_probe);

    log::info(
        "sched[a64]: first user switch tid=%u user_pt=0x%lx ttbr0=0x%lx ttbr1=0x%lx",
        next->tid, next->exec.user_pt_root, ttbr0, ttbr1
    );
    log::info(
        "sched[a64]: tcr=0x%lx t0sz=%lu(t0_va_bits=%lu) t1sz=%lu(t1_va_bits=%lu)",
        tcr, t0sz, va_bits_ttbr0, t1sz, va_bits_ttbr1
    );
    log::info(
        "sched[a64]: user stack probe=0x%lx mapped=%s phys=0x%lx l0=%u l1=%u l2=%u l3=%u",
        stack_probe, probe_mapped ? "yes" : "no", probe_phys,
        static_cast<uint32_t>(parts.l0_idx),
        static_cast<uint32_t>(parts.l1_idx),
        static_cast<uint32_t>(parts.l2_idx),
        static_cast<uint32_t>(parts.l3_idx)
    );

    log_user_walk_raw(next->exec.user_pt_root, stack_probe, "user_pt_root");

    paging::at_s1e0r(stack_probe);
    uint64_t par = paging::read_par_el1();
    bool fault = (par & 1ULL) != 0;
    if (fault) {
        uint64_t fst = (par >> 1) & 0x3FULL;
        log::warn(
            "sched[a64]: AT S1E0R fault for stack_probe=0x%lx PAR_EL1=0x%016lx FST=0x%02lx",
            stack_probe, par, fst
        );
    } else {
        uint64_t pa_base = par & DESC_ADDR_MASK_4K;
        uint64_t pa = pa_base | (stack_probe & 0xFFFULL);
        log::info(
            "sched[a64]: AT S1E0R ok for stack_probe=0x%lx PAR_EL1=0x%016lx -> PA=0x%lx",
            stack_probe, par, pa
        );
    }
}

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
    if (paging::get_kernel_pt_root() != next->exec.pt_root) {
        paging::set_kernel_pt_root(next->exec.pt_root);
        paging::flush_tlb_all();
    }
    paging::write_ttbr0_el1(next->exec.user_pt_root);
    log_first_user_switch_diagnostics(next);
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
    cpu::write_tls_base(next->exec.tls_base);
    arch_post_switch(next);
    // Prevent early off-CPU publication while trap exit still depends on prev context.
    defer_off_cpu_finalize(prev);
}

} // namespace sched
