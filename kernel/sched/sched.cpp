#include "sched/sched.h"
#include "sched/sched_internal.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "sched/sched_policy.h"
#include "sched/runqueue.h"
#include "sched/fpu.h"
#include "signals/signal.h"
#include "dynpriv/dynpriv.h"
#include "percpu/percpu.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "mm/kva.h"
#include "mm/mm.h"
#include "mm/paging.h"
#include "common/logging.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "smp/smp.h"
#include "hw/cpu.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "rc/reaper.h"
#include "exec/elf.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "common/string.h"
#include "resource/resource.h"
#include "resource/providers/proc_provider.h"
#include "fs/node.h"
#include "mm/uaccess.h"
#include "sync/futex.h"

DEFINE_PER_CPU(sched::task*, current_task);
DEFINE_PER_CPU(bool, percpu_is_elevated);
DEFINE_PER_CPU(uint32_t, percpu_cpu_id);
static DEFINE_PER_CPU(sched::task*, pending_off_cpu_task);
static DEFINE_PER_CPU(uint64_t, cpu_tlb_sync_epoch);

static DEFINE_PER_CPU(sched::runqueue, cpu_rq);

static DEFINE_PER_CPU(sched::cpu_accounting_stats, cpu_accounting);

static sync::atomic<uint32_t> g_next_tid{1};
static sync::atomic<uint32_t> g_pending_tlb_sync_tickets;

static sync::atomic<uint32_t> g_lb_next_cpu{0};

namespace sched {

__PRIVILEGED_CODE void thread_group::ref_destroy(thread_group* self) {
    heap::kfree_delete(self);
}

void init_default_rlimits(rlimit_pair* limits) {
    for (uint32_t i = 0; i < RLIMIT_COUNT; i++) {
        limits[i] = { RLIM_INFINITY, RLIM_INFINITY };
    }

    uint64_t stack_bytes = mm::USER_STACK_MAX_PAGES * pmm::PAGE_SIZE;
    limits[RLIMIT_STACK]  = { stack_bytes, stack_bytes };
    limits[RLIMIT_NOFILE] = { resource::MAX_TASK_HANDLES, resource::MAX_TASK_HANDLES };
}

__PRIVILEGED_CODE void task::ref_destroy(task* self) {
    rc::reaper::defer(&self->reaper_node);
}

constexpr size_t TASK_STACK_PAGES = 4;
constexpr uint16_t TASK_GUARD_PAGES = 1;

constexpr size_t SYSTEM_STACK_PAGES = 4;
constexpr uint16_t SYSTEM_GUARD_PAGES = 1;

constexpr uint64_t TLB_SYNC_CPU_IGNORED = ~0ULL;

constexpr uint32_t TEARDOWN_BATCH_SIZE = 16;

constexpr uint64_t AT_NULL   = 0;
constexpr uint64_t AT_PHDR   = 3;
constexpr uint64_t AT_PHENT  = 4;
constexpr uint64_t AT_PHNUM  = 5;
constexpr uint64_t AT_PAGESZ = 6;

static uint32_t load_cleanup_stage(const task* t) {
    return t->cleanup_stage.load_acquire();
}

static void store_cleanup_stage(task* t, uint32_t stage) {
    t->cleanup_stage.store_release(stage);
}

#ifdef DEBUG
[[noreturn]] __PRIVILEGED_CODE static void panic_invalid_privilege_state(
    const char* site
) {
    cpu::irq_disable();
    log::panic_write(
        "sched: invalid privilege state at %s: percpu_is_elevated dropped during switch teardown",
        site
    );
    for (;;) {
        cpu::halt();
    }
}

__PRIVILEGED_CODE static inline void assert_switch_privilege_state(
    const char* site
) {
    if (!this_cpu(percpu_is_elevated)) {
        panic_invalid_privilege_state(site);
    }
}
#endif

/**
 * Runs only after the last counted reference dropped. Registry lookups can
 * still find the task until the removal below, its poisoned refcount turns
 * them away. The TLB sync wait covers the freed stack pages, nothing else.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static rc::reaper::cleanup_result reap_task(sched::task* t) {
    uint32_t stage = load_cleanup_stage(t);
    if (stage < TASK_CLEANUP_STAGE_SCHEDULER_DETACHED) {
        return rc::reaper::RETRY_LATER;
    }

    if (sync::atomic_ref<uint32_t>{t->exec.on_cpu}.load_acquire()) {
        return rc::reaper::RETRY_LATER;
    }

    uint32_t cpu_count = smp::cpu_count();
    if (stage == TASK_CLEANUP_STAGE_SCHEDULER_DETACHED) {
        for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
            smp::cpu_info* info = smp::get_cpu_info(cpu);
            if (!info || info->state.load_acquire() != smp::CPU_ONLINE) {
                t->tlb_sync_ticket.cpu_epoch_snapshot[cpu] = TLB_SYNC_CPU_IGNORED;
                continue;
            }

            t->tlb_sync_ticket.cpu_epoch_snapshot[cpu] =
                sync::atomic_ref<uint64_t>{per_cpu_on(cpu_tlb_sync_epoch, cpu)}.load_acquire();
        }

        t->tlb_sync_ticket.armed.store_release(1);
        g_pending_tlb_sync_tickets.fetch_add_acq_rel(1);
        store_cleanup_stage(t, TASK_CLEANUP_STAGE_WAITING_FOR_TLB_SYNC);
        return rc::reaper::RETRY_LATER;
    }

    if (stage == TASK_CLEANUP_STAGE_WAITING_FOR_TLB_SYNC) {
        if (t->tlb_sync_ticket.armed.load_acquire() == 0) {
            return rc::reaper::RETRY_LATER;
        }

        for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
            if (t->tlb_sync_ticket.cpu_epoch_snapshot[cpu] == TLB_SYNC_CPU_IGNORED) {
                continue;
            }

            uint64_t epoch = sync::atomic_ref<uint64_t>{per_cpu_on(cpu_tlb_sync_epoch, cpu)}.load_acquire();
            if ((epoch - t->tlb_sync_ticket.cpu_epoch_snapshot[cpu]) == 0) {
                return rc::reaper::RETRY_LATER;
            }
        }

        g_pending_tlb_sync_tickets.fetch_sub_acq_rel(1);
        store_cleanup_stage(t, TASK_CLEANUP_STAGE_READY_TO_RECLAIM);
    }

    if (load_cleanup_stage(t) != TASK_CLEANUP_STAGE_READY_TO_RECLAIM) {
        return rc::reaper::RETRY_LATER;
    }

    g_task_registry.remove(*t);
    resource::release_task_handles(t);

    if (t->cwd) {
        if (t->cwd->release()) {
            fs::node::ref_destroy(t->cwd);
        }
        t->cwd = nullptr;
    }

    if (t->exec.mm_ctx) {
        mm::mm_context_release(t->exec.mm_ctx);
        t->exec.mm_ctx = nullptr;
        t->exec.user_pt_root = 0;
    }

    if (t->group) {
        if (t->group->release()) {
            thread_group::ref_destroy(t->group);
        }
        t->group = nullptr;
    }

    vmm::free(t->task_stack_base);
    vmm::free(t->sys_stack_base);
    heap::kfree_delete(t);

    return rc::reaper::DONE;
}

__PRIVILEGED_CODE static rc::reaper::cleanup_result reap_task_thunk(
    rc::reaper::dead_node* node
) {
    return rc::reaper::reaper_thunk<sched::task, reap_task>(node);
}

task* current() {
    return this_cpu(current_task);
}

// A pending SIGKILL bit is the task's "must terminate" marker
static inline bool task_kill_bit_set(const task* t) {
    return (t->sig.pending.load_acquire()
            & signals::sig_bit(signals::SIGKILL)) != 0;
}

bool is_kill_pending() {
    task* t = current();
    return t && task_kill_bit_set(t);
}

__PRIVILEGED_CODE void force_wake_for_kill(task* t) {
    t->sig.pending.fetch_or_acq_rel(signals::sig_bit(signals::SIGKILL));

    // Pairs with block_task_interrupted: the wake below sees BLOCKED,
    // or the blocker's interrupt check sees the SIGKILL bit. Never neither.
    sync::atomic_fence_seq_cst();
    timer::cancel_sleep(t);
    wake(t);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void prepare_to_block_task() {
    current()->state.store_release(TASK_STATE_BLOCKED);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool block_task_interrupted() {
    // Fence pairs with force_wake_for_kill and signals wake_for_signal.
    sync::atomic_fence_seq_cst();
    return signals::interrupt_pending(current());
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void cancel_block_task() {
    task* self = current();
    uint32_t expected = TASK_STATE_BLOCKED;
    if (!self->state.cmpxchg_strong_acq_rel(expected, TASK_STATE_RUNNING)) {
        // A wake already claimed this task READY and will requeue it once
        // it is off-CPU. Yield so that handoff can complete.
        yield();
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void record_cpu_tick(task* prev) {
    cpu_accounting_stats& stats = this_cpu(cpu_accounting);
    runqueue& rq = this_cpu(cpu_rq);

    // Only this CPU's tick path writes these counters, remote readers
    // use relaxed atomic loads
    uint64_t* counter = (prev == rq.idle_task) ? &stats.idle_ticks
                                               : &stats.busy_ticks;
    sync::atomic_ref<uint64_t>{*counter}.store_relaxed(*counter + 1);
    prev->run_ticks.store_relaxed(prev->run_ticks.load_relaxed() + 1);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE cpu_accounting_stats read_cpu_accounting_stats(uint32_t cpu_id) {
    cpu_accounting_stats& stats = per_cpu_on(cpu_accounting, cpu_id);
    cpu_accounting_stats out;
    out.busy_ticks = sync::atomic_ref<uint64_t>{stats.busy_ticks}.load_relaxed();
    out.idle_ticks = sync::atomic_ref<uint64_t>{stats.idle_ticks}.load_relaxed();
    out.tick_hz = timer::tick_hz();
    return out;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void finalize_pending_off_cpu() {
    task* pending = this_cpu(pending_off_cpu_task);
    if (!pending) {
        return;
    }

    this_cpu(pending_off_cpu_task) = nullptr;
    sync::atomic_ref<uint32_t>{pending->exec.on_cpu}.store_release(0);
    cpu::send_event();

    if (load_cleanup_stage(pending) == TASK_CLEANUP_STAGE_SCHEDULER_DETACHED) {
        // Reclamation must not begin before the off-CPU store above is
        // visible, so the reference the task was created with drops here.
        if (pending->release()) {
            task::ref_destroy(pending);
        }
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void defer_off_cpu_finalize(task* prev) {
    if (!prev) {
        return;
    }

    if (this_cpu(pending_off_cpu_task)) {
        finalize_pending_off_cpu();
    }
    this_cpu(pending_off_cpu_task) = prev;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void advance_cpu_tlb_sync_epoch() {
    if (g_pending_tlb_sync_tickets.load_acquire() == 0) {
        return;
    }

    paging::flush_tlb_all();
    sync::atomic_ref<uint64_t>{this_cpu(cpu_tlb_sync_epoch)}.fetch_add_release(1);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint32_t load_balance_select_cpu() {
    uint32_t online = smp::online_count();
    if (online <= 1) return 0;

    uint32_t target = g_lb_next_cpu.fetch_add_relaxed(1) % online;
    uint32_t total = smp::cpu_count();
    uint32_t seen = 0;
    for (uint32_t i = 0; i < total; i++) {
        smp::cpu_info* info = smp::get_cpu_info(i);
        if (info && info->state.load_acquire() == smp::CPU_ONLINE) {
            if (seen == target) return i;
            seen++;
        }
    }
    return 0;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task* pick_next_and_switch(task* prev) {
#ifdef DEBUG
    assert_switch_privilege_state("pick_next_and_switch:entry");
#endif

    runqueue& rq = this_cpu(cpu_rq);

    sync::irq_state irq = sync::spin_lock_irqsave(rq.lock);

    // Only re-enqueue if prev was running (not dead, blocked, or already woken)
    if (prev != rq.idle_task && prev->state.load_relaxed() == TASK_STATE_RUNNING) {
        prev->state.store_relaxed(TASK_STATE_READY);
        rq.policy->enqueue(prev);
        rq.nr_running++;
    }

    // Dead task is now scheduler-detached and can enter deferred cleanup flow.
    if (prev != rq.idle_task && prev->state.load_relaxed() == TASK_STATE_DEAD) {
        store_cleanup_stage(prev, TASK_CLEANUP_STAGE_SCHEDULER_DETACHED);
    }

    task* next = rq.policy->pick_next();
    if (next) {
        rq.nr_running--;
    } else {
        next = rq.idle_task;
    }

    next->state.store_relaxed(TASK_STATE_RUNNING);
    this_cpu(current_task) = next;
    this_cpu(current_task_exec) = &next->exec;
    // Runtime elevation state remains true while trap/syscall teardown continues.
    // Return-boundary code restores percpu_is_elevated from the selected task's
    // TASK_FLAG_ELEVATED after switch teardown is complete.
#ifdef DEBUG
    assert_switch_privilege_state("pick_next_and_switch:post-select");
#endif

    sync::spin_unlock_irqrestore(rq.lock, irq);

    return next;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enqueue(task* t) {
    uint32_t expected = TASK_STATE_CREATED;
    if (!t->state.cmpxchg_strong_acq_rel(expected, TASK_STATE_READY)) {
        log::warn("sched: enqueue rejected tid=%u (state=%u)", t->tid, expected);
        return;
    }

    uint32_t cpu = load_balance_select_cpu();
    t->exec.cpu = cpu;
    runqueue& rq = per_cpu_on(cpu_rq, cpu);

    sync::irq_state irq = sync::spin_lock_irqsave(rq.lock);
    rq.policy->enqueue(t);
    rq.nr_running++;
    sync::spin_unlock_irqrestore(rq.lock, irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enqueue_on(task* t, uint32_t cpu_id) {
    uint32_t expected = TASK_STATE_CREATED;
    if (!t->state.cmpxchg_strong_acq_rel(expected, TASK_STATE_READY)) {
        log::warn("sched: enqueue_on rejected tid=%u (state=%u)", t->tid, expected);
        return;
    }

    t->exec.cpu = cpu_id;
    runqueue& rq = per_cpu_on(cpu_rq, cpu_id);

    sync::irq_state irq = sync::spin_lock_irqsave(rq.lock);
    rq.policy->enqueue(t);
    rq.nr_running++;
    sync::spin_unlock_irqrestore(rq.lock, irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE rc::strong_ref<task> task_ref(task* t) {
    return rc::strong_ref<task>::try_from_raw(t);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE rc::strong_ref<task> task_ref_by_tid(uint32_t tid) {
    sync::irq_state irq = g_task_registry.lock();
    rc::strong_ref<task> ref = task_ref(g_task_registry.find_locked(tid));
    g_task_registry.unlock(irq);

    return ref;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake(task* t) {
    uint32_t expected = TASK_STATE_BLOCKED;
    if (!t->state.cmpxchg_strong_acq_rel(expected, TASK_STATE_READY)) {
        return;
    }

    uint32_t task_cpu = sync::atomic_ref<uint32_t>{t->exec.cpu}.load_relaxed();
    if (task_cpu != percpu::current_cpu_id()) {
        while (sync::atomic_ref<uint32_t>{t->exec.on_cpu}.load_acquire()) {
            cpu::relax();
        }
    }

    runqueue& rq = per_cpu_on(cpu_rq, task_cpu);

    sync::irq_state irq = sync::spin_lock_irqsave(rq.lock);
    rq.policy->enqueue(t);
    rq.nr_running++;
    sync::spin_unlock_irqrestore(rq.lock, irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint64_t sleep_ns(uint64_t ns) {
    if (ns == 0) {
        yield();
        return 0;
    }

    task* self = current();
    if (self->exec.flags & TASK_FLAG_IDLE) {
        return 0;
    }

    uint64_t deadline = clock::now_ns() + ns;

    prepare_to_block_task();
    timer::schedule_sleep(self, deadline);

    if (block_task_interrupted()) {
        // Interrupted before or during sleep entry: do not serve the sleep.
        timer::cancel_sleep(self);
        cancel_block_task();
    } else {
        yield();
    }

    uint64_t now = clock::now_ns();
    return deadline > now ? deadline - now : 0;
}

__PRIVILEGED_CODE void sleep_us(uint64_t us) {
    sleep_ns(us * 1000ULL);
}

__PRIVILEGED_CODE void sleep_ms(uint64_t ms) {
    sleep_ns(ms * 1000000ULL);
}

[[noreturn]] void exit(int exit_code) {
    RUN_ELEVATED({
        sched::task* task = current();

        // Zero the registered thread id address and wake one joiner
        // while the address space is still mapped
        if (task->clear_child_tid) {
            uint32_t zero = 0;

            if (mm::uaccess::copy_to_user(
                    reinterpret_cast<void*>(task->clear_child_tid),
                    &zero, sizeof(zero)) == mm::uaccess::OK) {
                sync::futex_wake(task->clear_child_tid, 1);
            }

            task->clear_child_tid = 0;
        }

        // Thread group handling: leader kills all threads, non-leader removes itself
        if (task->group) {
            thread_group* tg = task->group;

            if (tg->leader == task) {
                // Reaped never-started threads report the recorded group exit
                // status or signal so every member exposes the same death cause
                int32_t reap_status = TASK_KILL_STATUS;

                uint32_t ges = tg->group_exit_status.load_acquire();
                uint32_t es = tg->sig.exit_signal.load_acquire();

                if (ges != 0) {
                    reap_status = static_cast<int32_t>(ges & 0xFF00u);
                } else if (es != 0) {
                    reap_status = static_cast<int32_t>(es) & 0x7F;
                }

                // Threads are handled in batches: kill references and orphaned
                // proc resources collected under tg->lock are woken only after
                // it drops, keeping the off-CPU spin in wake outside the lock
                for (;;) {
                    rc::strong_ref<sched::task> kill_batch[TEARDOWN_BATCH_SIZE];
                    rc::strong_ref<resource::proc_provider::proc_resource> pr_batch[TEARDOWN_BATCH_SIZE];
                    uint32_t kills = 0;
                    uint32_t prs = 0;
                    bool rescan = false;

                    sync::irq_state irq = sync::spin_lock_irqsave(tg->lock);
                    auto it = tg->threads.begin();
                    auto end = tg->threads.end();

                    while (it != end) {
                        sched::task& thread = *it;
                        ++it; // advance before potential removal

                        if (kills == TEARDOWN_BATCH_SIZE || prs == TEARDOWN_BATCH_SIZE) {
                            rescan = true;
                            break;
                        }

                        uint32_t expected = TASK_STATE_CREATED;
                        if (thread.state.cmpxchg_strong_acq_rel(expected,
                                TASK_STATE_DEAD)) {
                            tg->threads.remove(&thread);
                            tg->thread_count--;

                            if (thread.proc_res) {
                                auto* pr = thread.proc_res;

                                sync::irq_state pr_irq = sync::spin_lock_irqsave(pr->lock);
                                pr->wait_status = reap_status;
                                pr->exited = true;
                                pr->child = nullptr;
                                sync::spin_unlock_irqrestore(pr->lock, pr_irq);

                                // The thread's resource reference moves to the
                                // batch and is released after the deferred wake
                                thread.proc_res = nullptr;
                                pr_batch[prs++] = rc::strong_ref<resource::proc_provider::proc_resource>::adopt(pr);
                            }

                            store_cleanup_stage(&thread, TASK_CLEANUP_STAGE_SCHEDULER_DETACHED);
                            if (thread.release()) {
                                task::ref_destroy(&thread);
                            }
                        } else if (!(thread.sig.pending.load_acquire()
                                     & signals::sig_bit(signals::SIGKILL))) {
                            // Setting the kill bit under tg->lock marks the
                            // thread handled, so a rescan cannot batch it twice
                            thread.sig.pending.fetch_or_acq_rel(
                                signals::sig_bit(signals::SIGKILL));

                            kill_batch[kills++] = task_ref(&thread);
                        }
                    }

                    if (!rescan) {
                        tg->leader = nullptr;
                    }
                    sync::spin_unlock_irqrestore(tg->lock, irq);

                    for (uint32_t i = 0; i < prs; i++) {
                        sync::wake_all(pr_batch[i]->wait_queue);
                    }

                    for (uint32_t i = 0; i < kills; i++) {
                        if (kill_batch[i]) {
                            force_wake_for_kill(kill_batch[i].ptr());
                        }
                    }

                    if (!rescan) {
                        break;
                    }
                }
            } else if (task->group_link.is_linked()) {
                sync::irq_state irq = sync::spin_lock_irqsave(tg->lock);
                tg->threads.remove(task);
                tg->thread_count--;
                sync::spin_unlock_irqrestore(tg->lock, irq);
            }
        }

        if (task->proc_res) {
            auto* pr = task->proc_res;

            uint32_t ges = task->group
                ? task->group->group_exit_status.load_acquire()
                : 0;
            bool announce = false;

            sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);
            if (!pr->detached) {
                // A recorded exit_group status overrides the kill bit so
                // the process reports the exit code, not a signal death
                if (ges != 0) {
                    pr->wait_status = static_cast<int32_t>(ges & 0xFF00u);
                } else if (task_kill_bit_set(task)) {
                    pr->wait_status = exit_code & 0x7F;
                } else {
                    pr->wait_status = (exit_code & 0xFF) << 8;
                }
                pr->exited = true;
                pr->child = nullptr;
                announce = true;
            } else {
                pr->child = nullptr;
            }
            sync::spin_unlock_irqrestore(pr->lock, irq);

            // Waiters recheck pr->exited under pr->lock, so waking after
            // the lock drops cannot lose the wakeup
            if (announce) {
                sync::wake_all(pr->wait_queue);
            }

            task->proc_res = nullptr;
            if (pr->release()) {
                resource::proc_provider::proc_resource::ref_destroy(pr);
            }
        }

        store_cleanup_stage(task, TASK_CLEANUP_STAGE_EXIT_REQUESTED);
        task->state.store_relaxed(TASK_STATE_DEAD);
        task->exit_code = exit_code;
    });
    yield();
    __builtin_unreachable();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task* create_kernel_task(
    void (*entry)(void*), void* arg, const char* name, uint32_t flags
) {
    task* t = heap::kalloc_new<task>();
    if (!t) {
        log::error("sched: failed to allocate task struct");
        return nullptr;
    }

    bool elevated = (flags & TASK_FLAG_ELEVATED) != 0;
    kva::tag stack_tag = elevated ? kva::tag::privileged_stack : kva::tag::unprivileged_stack;

    uintptr_t task_stack_base = 0;
    uintptr_t task_stack_top = 0;
    if (vmm::alloc_stack(TASK_STACK_PAGES, TASK_GUARD_PAGES,
            stack_tag, task_stack_base, task_stack_top) != vmm::OK) {
        log::error("sched: failed to allocate task stack");
        heap::kfree_delete(t);
        return nullptr;
    }

    uintptr_t sys_stack_base = 0;
    uintptr_t sys_stack_top = 0;
    if (vmm::alloc_stack(SYSTEM_STACK_PAGES, SYSTEM_GUARD_PAGES,
            kva::tag::privileged_stack, sys_stack_base, sys_stack_top) != vmm::OK) {
        log::error("sched: failed to allocate system stack");
        vmm::free(task_stack_base);
        heap::kfree_delete(t);
        return nullptr;
    }

    t->exec.flags = TASK_FLAG_KERNEL | TASK_FLAG_CAN_ELEVATE | TASK_FLAG_PREEMPTIBLE | flags;
    t->exec.cpu = 0;
    t->exec.task_stack_top = task_stack_top;
    t->exec.system_stack_top = sys_stack_top;

    t->exec.pt_root = paging::get_kernel_pt_root();
    t->exec.user_pt_root = 0;
    t->exec.mm_ctx = nullptr;
    t->exec.tls_base = 0;

    t->task_stack_base = task_stack_base;
    t->sys_stack_base = sys_stack_base;

    // Zero cpu_ctx, then set arch-specific initial state
    uint8_t* ctx_bytes = reinterpret_cast<uint8_t*>(&t->exec.cpu_ctx);
    for (size_t i = 0; i < sizeof(thread_cpu_context); i++) {
        ctx_bytes[i] = 0;
    }
    arch_init_task_context(t, entry, arg);

    t->exec.on_cpu = 0;

    t->tid = g_next_tid.fetch_add_relaxed(1);
    t->state.store_relaxed(TASK_STATE_CREATED);
    t->task_registry_link = {};
    t->sched_link = {};
    t->wait_link = {};
    t->timer_link = {};
    t->timer_deadline = 0;

    string::memcpy(t->name, name, string::strnlen(name, TASK_NAME_MAX - 1));
    t->name[string::strnlen(name, TASK_NAME_MAX - 1)] = '\0';

    t->cleanup_stage.store_relaxed(TASK_CLEANUP_STAGE_ACTIVE);
    t->tlb_sync_ticket.armed.store_relaxed(0);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        t->tlb_sync_ticket.cpu_epoch_snapshot[i] = 0;
    }

    fpu::init_state(&t->exec.fpu_ctx);
    t->reaper_node.init(reap_task_thunk);

    if (resource::init_task_handles(t) != resource::OK) {
        log::error("sched: failed to allocate kernel task handle table");
        vmm::free(task_stack_base);
        vmm::free(sys_stack_base);
        heap::kfree_delete(t);
        return nullptr;
    }

    t->proc_res = nullptr;
    t->cwd = nullptr;
    t->clear_child_tid = 0;
    t->sig.blocked.store_relaxed(0);
    t->sig.pending.store_relaxed(0);
    t->group = nullptr;
    t->group_link = {};

    g_task_registry.insert(t);

    return t;
}

/**
 * Build the Linux-compatible initial stack layout (argc, argv, envp, auxv)
 * that musl's _start expects. Writes data into the last page of the
 * already-mapped user stack via the kernel HHDM mapping.
 *
 * @param user_argc Number of user-provided args (excluding program name).
 * @param user_argv Kernel-copied argument strings, or nullptr for none.
 * @param user_envc Number of environment strings.
 * @param user_envp Kernel-copied environment strings, or nullptr for none.
 * @return 16-byte-aligned user stack pointer pointing to argc, or 0 on error.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uintptr_t setup_user_stack(
    pmm::phys_addr_t last_page_phys,
    uintptr_t stack_top,
    const exec::loaded_image& image,
    const char* name,
    int user_argc,
    const char* const* user_argv,
    int user_envc,
    const char* const* user_envp
) {
    uint8_t* page_kva = static_cast<uint8_t*>(paging::phys_to_virt(last_page_phys));
    uintptr_t page_base_va = pmm::page_align_down(stack_top - 1);
    auto write = [&](uintptr_t user_va, const void* data, size_t len) {
        size_t offset = user_va - page_base_va;
        string::memcpy(page_kva + offset, data, len);
    };

    int total_argc = 1 + user_argc; // argv[0] = name, argv[1..] = user args

    constexpr size_t MAX_ARGV_PTRS = 1 + MAX_ARG_STRINGS; // program name + user args
    constexpr size_t MAX_ENVP_PTRS = MAX_ARG_STRINGS;
    constexpr size_t AUXV_ENTRIES = 5;
    constexpr size_t AUXV_WORDS = AUXV_ENTRIES * 2;

    size_t struct_words = 1 + static_cast<size_t>(total_argc) + 1
                        + static_cast<size_t>(user_envc) + 1 + AUXV_WORDS;
    size_t struct_bytes = struct_words * sizeof(uint64_t);

    // Pre-compute total string space needed (8-byte aligned per arg)
    size_t name_len = string::strnlen(name, TASK_NAME_MAX - 1) + 1;
    size_t total_string_bytes = (name_len + 7) & ~7ULL;
    for (int i = 0; i < user_argc; i++) {
        size_t arg_len = string::strnlen(user_argv[i], MAX_ARG_STRLEN - 1) + 1;
        total_string_bytes += (arg_len + 7) & ~7ULL;
    }
    for (int i = 0; i < user_envc; i++) {
        size_t env_len = string::strnlen(user_envp[i], MAX_ARG_STRLEN - 1) + 1;
        total_string_bytes += (env_len + 7) & ~7ULL;
    }

    if (total_string_bytes + struct_bytes + 16 > pmm::PAGE_SIZE) {
        return 0;
    }

    // Write strings at the top of the page (growing downward from stack_top)
    uintptr_t str_cursor = stack_top;
    uintptr_t argv_vas[MAX_ARGV_PTRS];

    // argv[0] = program name
    size_t name_padded = (name_len + 7) & ~7ULL;
    str_cursor -= name_padded;
    write(str_cursor, name, name_len);
    argv_vas[0] = str_cursor;

    // argv[1..user_argc] = user-provided args
    for (int i = 0; i < user_argc; i++) {
        size_t arg_len = string::strnlen(user_argv[i], MAX_ARG_STRLEN - 1) + 1;
        size_t arg_padded = (arg_len + 7) & ~7ULL;
        str_cursor -= arg_padded;
        write(str_cursor, user_argv[i], arg_len);
        argv_vas[1 + i] = str_cursor;
    }

    uintptr_t envp_vas[MAX_ENVP_PTRS];
    for (int i = 0; i < user_envc; i++) {
        size_t env_len = string::strnlen(user_envp[i], MAX_ARG_STRLEN - 1) + 1;
        size_t env_padded = (env_len + 7) & ~7ULL;
        str_cursor -= env_padded;
        write(str_cursor, user_envp[i], env_len);
        envp_vas[i] = str_cursor;
    }

    uintptr_t sp = (str_cursor - struct_bytes) & ~0xFULL;

    uint64_t data[1 + MAX_ARGV_PTRS + 1 + MAX_ENVP_PTRS + 1 + AUXV_WORDS];
    size_t idx = 0;
    data[idx++] = static_cast<uint64_t>(total_argc);

    for (int i = 0; i < total_argc; i++) {
        data[idx++] = argv_vas[i];
    }
    data[idx++] = 0; // argv terminator (NULL)

    for (int i = 0; i < user_envc; i++) {
        data[idx++] = envp_vas[i];
    }
    data[idx++] = 0; // envp terminator (NULL)

    data[idx++] = AT_PAGESZ; data[idx++] = pmm::PAGE_SIZE;
    data[idx++] = AT_PHDR;   data[idx++] = image.phdr_vaddr;
    data[idx++] = AT_PHENT;  data[idx++] = image.phentsize;
    data[idx++] = AT_PHNUM;  data[idx++] = image.phdr_vaddr ? image.phnum : 0;
    data[idx++] = AT_NULL;   data[idx++] = 0;

    write(sp, data, idx * sizeof(uint64_t));
    return sp;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task* create_user_task(
    exec::loaded_image* image, const char* name,
    int argc, const char* const* argv,
    int envc, const char* const* envp
) {
    if (!image || !image->mm_ctx) {
        log::error("sched: invalid loaded image for user task");
        return nullptr;
    }

    mm::mm_context* mm_ctx = image->mm_ctx;

    task* t = heap::kalloc_new<task>();
    if (!t) {
        log::error("sched: failed to allocate user task struct");
        return nullptr;
    }

    // System stack in kernel VA (for interrupt handling)
    uintptr_t sys_stack_base = 0;
    uintptr_t sys_stack_top = 0;
    if (vmm::alloc_stack(SYSTEM_STACK_PAGES, SYSTEM_GUARD_PAGES,
            kva::tag::privileged_stack, sys_stack_base, sys_stack_top) != vmm::OK) {
        log::error("sched: failed to allocate system stack for user task");
        heap::kfree_delete(t);
        return nullptr;
    }

    // Stack region layout: a single coalesced MM_MAP_STACK vma spanning
    // USER_STACK_MAX_PAGES at the top of user VA. The bottom portion is
    // reserved lazily (no eager pages) so userland faults grow it on demand.
    // The top USER_STACK_PAGES window is eagerly mapped so the kernel can
    // write argv/envp into it before the user task ever runs.
    uintptr_t stack_max_base = mm::USER_STACK_TOP - mm::USER_STACK_MAX_PAGES * pmm::PAGE_SIZE;
    uintptr_t eager_base     = mm::USER_STACK_TOP - mm::USER_STACK_PAGES     * pmm::PAGE_SIZE;
    size_t    lazy_bytes     = (mm::USER_STACK_MAX_PAGES - mm::USER_STACK_PAGES) * pmm::PAGE_SIZE;
    size_t    eager_bytes    = mm::USER_STACK_PAGES     * pmm::PAGE_SIZE;
    size_t    total_bytes    = mm::USER_STACK_MAX_PAGES * pmm::PAGE_SIZE;

    uint32_t base_stack_flags = mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS |
        mm::MM_MAP_FIXED | mm::MM_MAP_STACK;

    // Reserve the lower (lazy) portion of the stack VMA - no eager pages.
    uintptr_t reserved_addr = 0;
    int32_t lazy_rc = mm::mm_context_map_anonymous(
        mm_ctx,
        stack_max_base,
        lazy_bytes,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE,
        base_stack_flags | mm::MM_MAP_LAZY,
        &reserved_addr
    );
    if (lazy_rc != mm::MM_CTX_OK) {
        log::error("sched: failed to reserve user stack VMA (rc=%d)", lazy_rc);
        vmm::free(sys_stack_base);
        heap::kfree_delete(t);
        return nullptr;
    }

    // Eagerly map the top window for argv/envp setup. Coalescing merges it
    // with the lazy reservation into one stack vma up to USER_STACK_TOP.
    uintptr_t mapped_stack_addr = 0;
    int32_t eager_rc = mm::mm_context_map_anonymous(
        mm_ctx,
        eager_base,
        eager_bytes,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE,
        base_stack_flags,
        &mapped_stack_addr
    );
    if (eager_rc != mm::MM_CTX_OK) {
        log::error("sched: failed to map user stack window (rc=%d)", eager_rc);
        mm::mm_context_unmap(mm_ctx, stack_max_base, lazy_bytes);
        vmm::free(sys_stack_base);
        heap::kfree_delete(t);
        return nullptr;
    }

    pmm::phys_addr_t last_stack_page_phys =
        paging::get_physical(mm::USER_STACK_TOP - pmm::PAGE_SIZE, mm_ctx->pt_root);
    if (last_stack_page_phys == 0) {
        log::error("sched: failed to resolve user stack top page");
        mm::mm_context_unmap(mm_ctx, stack_max_base, total_bytes);
        vmm::free(sys_stack_base);
        heap::kfree_delete(t);
        return nullptr;
    }

    uintptr_t user_sp = setup_user_stack(
        last_stack_page_phys, mm::USER_STACK_TOP, *image, name,
        argc, argv, envc, envp);
    if (user_sp == 0) {
        log::error("sched: user stack setup failed (argv/envp too large?)");
        mm::mm_context_unmap(mm_ctx, stack_max_base, total_bytes);
        vmm::free(sys_stack_base);
        heap::kfree_delete(t);
        return nullptr;
    }

    t->exec.flags = TASK_FLAG_PREEMPTIBLE;
    t->exec.cpu = 0;
    t->exec.task_stack_top = user_sp;
    t->exec.system_stack_top = sys_stack_top;

    t->exec.pt_root = paging::supervisor_pt_root_for_user_task(mm_ctx->pt_root);
    t->exec.user_pt_root = mm_ctx->pt_root;
    t->exec.mm_ctx = mm_ctx;
    t->exec.tls_base = 0;

    t->task_stack_base = 0; // user stack is not VMM-allocated
    t->sys_stack_base = sys_stack_base;

    uint8_t* ctx_bytes = reinterpret_cast<uint8_t*>(&t->exec.cpu_ctx);
    for (size_t i = 0; i < sizeof(thread_cpu_context); i++) {
        ctx_bytes[i] = 0;
    }
    auto entry = reinterpret_cast<void(*)(void*)>(image->entry_point);
    arch_init_task_context(t, entry, nullptr);

    t->exec.on_cpu = 0;
    fpu::init_state(&t->exec.fpu_ctx);

    t->tid = g_next_tid.fetch_add_relaxed(1);
    t->state.store_relaxed(TASK_STATE_CREATED);
    t->task_registry_link = {};
    t->sched_link = {};
    t->wait_link = {};
    t->timer_link = {};
    t->timer_deadline = 0;

    string::memcpy(t->name, name, string::strnlen(name, TASK_NAME_MAX - 1));
    t->name[string::strnlen(name, TASK_NAME_MAX - 1)] = '\0';

    t->cleanup_stage.store_relaxed(TASK_CLEANUP_STAGE_ACTIVE);
    t->tlb_sync_ticket.armed.store_relaxed(0);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        t->tlb_sync_ticket.cpu_epoch_snapshot[i] = 0;
    }

    t->reaper_node.init(reap_task_thunk);

    if (resource::init_task_handles(t) != resource::OK) {
        log::error("sched: failed to allocate process handle table");
        if (t->exec.mm_ctx) {
            mm::mm_context_release(t->exec.mm_ctx);
            t->exec.mm_ctx = nullptr;
            t->exec.user_pt_root = 0;
            image->mm_ctx = nullptr;
            image->pt_root = 0;
        }
        vmm::free(sys_stack_base);
        heap::kfree_delete(t);
        return nullptr;
    }

    t->proc_res = nullptr;
    t->cwd = nullptr;
    t->clear_child_tid = 0;
    t->sig.blocked.store_relaxed(0);
    t->sig.pending.store_relaxed(0);

    auto* tg = heap::kalloc_new<thread_group>();
    if (!tg) {
        log::error("sched: failed to allocate thread_group");
        resource::release_task_handles(t);
        if (t->exec.mm_ctx) {
            mm::mm_context_release(t->exec.mm_ctx);
            t->exec.mm_ctx = nullptr;
            t->exec.user_pt_root = 0;
            image->mm_ctx = nullptr;
            image->pt_root = 0;
        }
        vmm::free(sys_stack_base);
        heap::kfree_delete(t);
        return nullptr;
    }

    tg->lock = sync::SPINLOCK_INIT;
    tg->leader = t;
    tg->pid = t->tid;
    tg->threads.init();
    tg->thread_count = 0;
    tg->group_exit_status.store_relaxed(0);

    // POSIX inheritance: a new process joins its creator's process group
    task* creator = current();
    tg->group_id.store_relaxed((creator && creator->group)
        ? creator->group->group_id.load_acquire()
        : t->tid);

    // POSIX inheritance: resource limits copy from the creating process
    if (creator && creator->group) {
        sync::spin_lock(creator->group->lock);
        for (uint32_t i = 0; i < RLIMIT_COUNT; i++) {
            tg->rlimits[i] = creator->group->rlimits[i];
        }
        sync::spin_unlock(creator->group->lock);
    } else {
        init_default_rlimits(tg->rlimits);
    }

    t->group = tg; // task takes ownership of the initial ref (refcount=1)
    t->group_link = {};

    image->mm_ctx = nullptr;
    image->pt_root = 0;

    g_task_registry.insert(t);

    return t;
}

/**
 * Shared setup for native and clone threads. Initializes every task
 * field except the initial CPU context, which each entry point fills
 * in after this returns. The task joins the creator's thread group
 * and either snapshots or shares the creator's handle table.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static task* init_user_thread_core(
    task* creator, uintptr_t stack_top, bool share_files, const char* name
) {
    if (!creator || !creator->exec.mm_ctx || !creator->group ||
        !creator->handles) {
        log::error("sched: invalid creator provided for user thread creation");
        return nullptr;
    }

    task* t = heap::kalloc_new<task>();
    if (!t) {
        log::error("sched: failed to allocate user thread task struct");
        return nullptr;
    }

    // System stack in kernel VA
    uintptr_t sys_stack_base = 0;
    uintptr_t sys_stack_top = 0;

    if (vmm::alloc_stack(SYSTEM_STACK_PAGES, SYSTEM_GUARD_PAGES,
            kva::tag::privileged_stack, sys_stack_base, sys_stack_top) != vmm::OK) {
        log::error("sched: failed to allocate system stack for user thread task");
        heap::kfree_delete(t);
        return nullptr;
    }

    if (share_files) {
        // POSIX thread semantics, both tasks use one shared table
        creator->handles->add_ref();
        t->handles = creator->handles;
    } else {
        // Native thread semantics, the new task gets a private snapshot
        // copy of the creator's handle table under the source table lock
        if (resource::init_task_handles(t) != resource::OK) {
            log::error("sched: failed to allocate thread handle table");
            vmm::free(sys_stack_base);
            heap::kfree_delete(t);
            return nullptr;
        }

        sync::irq_lock_guard guard(creator->handles->lock);

        for (uint32_t i = 0; i < resource::MAX_TASK_HANDLES; i++) {
            const auto& src = creator->handles->entries[i];
            if (!src.used || !src.obj) {
                continue;
            }

            auto& dst = t->handles->entries[i];
            dst.used = true;
            dst.generation = src.generation;
            dst.flags = src.flags;
            dst.rights = src.rights;
            dst.type = src.type;

            dst.obj = src.obj;
            resource::resource_add_ref(dst.obj);
        }
    }

    t->exec.flags = TASK_FLAG_PREEMPTIBLE;
    t->exec.cpu = 0;
    t->exec.on_cpu = 0;
    t->exec.task_stack_top = stack_top;
    t->exec.system_stack_top = sys_stack_top;

    t->exec.pt_root = paging::supervisor_pt_root_for_user_task(creator->exec.mm_ctx->pt_root);
    t->exec.user_pt_root = creator->exec.mm_ctx->pt_root;
    t->exec.tls_base = creator->exec.tls_base;

    t->task_stack_base = 0; // user stack is not VMM-allocated
    t->sys_stack_base = sys_stack_base;

    creator->exec.mm_ctx->add_ref(); // thread takes a shared reference to the mm context
    t->exec.mm_ctx = creator->exec.mm_ctx;

    fpu::init_state(&t->exec.fpu_ctx);

    uint8_t* ctx_bytes = reinterpret_cast<uint8_t*>(&t->exec.cpu_ctx);
    for (size_t i = 0; i < sizeof(thread_cpu_context); i++) {
        ctx_bytes[i] = 0;
    }

    t->tid = g_next_tid.fetch_add_relaxed(1);
    t->state.store_relaxed(TASK_STATE_CREATED);
    t->exit_code = 0;
    t->cleanup_stage.store_relaxed(TASK_CLEANUP_STAGE_ACTIVE);
    t->clear_child_tid = 0;

    t->sig.pending.store_relaxed(0);
    t->sig.blocked.store_relaxed(creator->sig.blocked.load_acquire());

    string::memcpy(t->name, name, string::strnlen(name, TASK_NAME_MAX - 1));
    t->name[string::strnlen(name, TASK_NAME_MAX - 1)] = '\0';

    t->task_registry_link = {};
    t->sched_link = {};
    t->wait_link = {};
    t->timer_link = {};
    t->timer_deadline = 0;
    t->tlb_sync_ticket.armed.store_relaxed(0);

    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        t->tlb_sync_ticket.cpu_epoch_snapshot[i] = 0;
    }

    t->reaper_node.init(reap_task_thunk);

    // Join the process's thread group
    thread_group* tg = creator->group;
    tg->add_ref(); // thread takes a shared reference to the group

    t->group = tg;
    t->group_link = {};

    sync::irq_state irq = sync::spin_lock_irqsave(tg->lock);
    tg->threads.push_back(t);
    tg->thread_count++;
    sync::spin_unlock_irqrestore(tg->lock, irq);

    t->proc_res = nullptr;

    t->cwd = creator->cwd;
    if (t->cwd) {
        t->cwd->add_ref();
    }

    g_task_registry.insert(t);

    return t;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task* create_user_thread(
    task* creator, uintptr_t entry, uintptr_t arg,
    uintptr_t stack_top, const char* name
) {
#if defined(__x86_64__)
    // The task starts at a function entry, where the ABI expects the
    // 8 byte bias a call instruction would have left on the stack
    stack_top -= 0x8;
#endif

    task* t = init_user_thread_core(creator, stack_top, false, name);
    if (!t) {
        return nullptr;
    }

    arch_init_task_context(t, reinterpret_cast<void (*)(void *)>(entry), reinterpret_cast<void*>(arg));

    return t;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task* clone_user_thread(
    task* creator, uintptr_t stack_top, uintptr_t tls, bool set_tls,
    bool share_files
) {
    // The child resumes at the clone return point rather than at a
    // function entry, so the stack pointer is used exactly as passed
    task* t = init_user_thread_core(creator, stack_top, share_files,
                                    creator->name);
    if (!t) {
        return nullptr;
    }

    // Marks the thread as following POSIX process wide exit semantics,
    // unlike a native thread which exits on its own and stays joinable
    t->exec.flags |= TASK_FLAG_POSIX_THREAD;

    if (set_tls) {
        t->exec.tls_base = tls;
    }

    arch_init_clone_cpu_context(t);

    return t;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    task* idle = heap::kalloc_new<task>();
    if (!idle) {
        log::error("sched: failed to allocate idle task");
        return ERR_NO_MEM;
    }

    // Copy exec core from the existing boot task (byte copy to avoid memcpy)
    task_exec_core* boot_exec = this_cpu(current_task_exec);
    if (boot_exec) {
        auto* dst = reinterpret_cast<uint8_t*>(&idle->exec);
        auto* src = reinterpret_cast<const uint8_t*>(boot_exec);
        for (size_t i = 0; i < sizeof(task_exec_core); i++) {
            dst[i] = src[i];
        }
    }

    idle->exec.pt_root = paging::get_kernel_pt_root();
    idle->exec.user_pt_root = 0;
    idle->exec.mm_ctx = nullptr;
    idle->exec.flags |= TASK_FLAG_IDLE;

    idle->tid = 0;
    idle->state.store_relaxed(TASK_STATE_RUNNING);
    idle->task_stack_base = 0;
    idle->sys_stack_base = 0;

    idle->task_registry_link = {};
    idle->sched_link = {};
    idle->wait_link = {};
    idle->timer_link = {};
    idle->timer_deadline = 0;

    string::memcpy(idle->name, "idle", 4);
    idle->name[4] = '\0';

    idle->cleanup_stage.store_relaxed(TASK_CLEANUP_STAGE_ACTIVE);
    idle->tlb_sync_ticket.armed.store_relaxed(0);
    fpu::init_state(&idle->exec.fpu_ctx);

    if (resource::init_task_handles(idle) != resource::OK) {
        log::error("sched: failed to allocate idle task handle table");
        return ERR_NO_MEM;
    }

    idle->proc_res = nullptr;
    idle->cwd = nullptr;
    idle->clear_child_tid = 0;
    idle->sig.blocked.store_relaxed(0);
    idle->sig.pending.store_relaxed(0);
    idle->group = nullptr;
    idle->group_link = {};

    this_cpu(current_task) = idle;
    this_cpu(current_task_exec) = &idle->exec;
    this_cpu(percpu_is_elevated) = (idle->exec.flags & TASK_FLAG_ELEVATED) != 0;
    this_cpu(pending_off_cpu_task) = nullptr;
    this_cpu(cpu_tlb_sync_epoch) = 0;

    runqueue& rq = this_cpu(cpu_rq);
    rq.lock = sync::SPINLOCK_INIT;
    rq.nr_running = 0;
    rq.idle_task = idle;

    auto* policy = heap::kalloc_new<round_robin_policy>();
    if (!policy) {
        log::error("sched: failed to allocate scheduling policy");
        return ERR_NO_MEM;
    }

    policy->init();
    rq.policy = policy;

    if (g_task_registry.init() != 0) {
        log::error("sched: task registry init failed");
        return ERR_NO_MEM;
    }

    log::info("sched: initialized (round-robin, tid0=idle)");

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap(uint32_t cpu_id, uintptr_t task_stack_top,
                                  uintptr_t system_stack_top) {
    task* idle = heap::kalloc_new<task>();
    if (!idle) {
        return ERR_NO_MEM;
    }

    idle->exec.flags = TASK_FLAG_IDLE | TASK_FLAG_ELEVATED | TASK_FLAG_KERNEL
                     | TASK_FLAG_CAN_ELEVATE | TASK_FLAG_PREEMPTIBLE;
    idle->exec.cpu = cpu_id;
    idle->exec.on_cpu = 1;
    idle->exec.task_stack_top = task_stack_top;
    idle->exec.system_stack_top = system_stack_top;

    idle->exec.pt_root = paging::get_kernel_pt_root();
    idle->exec.user_pt_root = 0;
    idle->exec.mm_ctx = nullptr;
    idle->task_stack_base = 0;
    idle->sys_stack_base = 0;

    idle->tid = g_next_tid.fetch_add_relaxed(1);
    idle->state.store_relaxed(TASK_STATE_RUNNING);
    string::memcpy(idle->name, "idle", 4);
    idle->name[4] = '\0';
    idle->cleanup_stage.store_relaxed(TASK_CLEANUP_STAGE_ACTIVE);
    idle->tlb_sync_ticket.armed.store_relaxed(0);
    fpu::init_state(&idle->exec.fpu_ctx);

    if (resource::init_task_handles(idle) != resource::OK) {
        return ERR_NO_MEM;
    }

    idle->proc_res = nullptr;
    idle->cwd = nullptr;

    this_cpu(current_task) = idle;
    this_cpu(current_task_exec) = &idle->exec;
    this_cpu(percpu_is_elevated) = true;
    this_cpu(pending_off_cpu_task) = nullptr;
    this_cpu(cpu_tlb_sync_epoch) = 0;

    runqueue& rq = this_cpu(cpu_rq);
    rq.lock = sync::SPINLOCK_INIT;
    rq.nr_running = 0;
    rq.idle_task = idle;

    auto* policy = heap::kalloc_new<round_robin_policy>();
    if (!policy) {
        return ERR_NO_MEM;
    }

    policy->init();
    rq.policy = policy;

    return OK;
}

} // namespace sched
