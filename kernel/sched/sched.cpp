#include "sched/sched.h"
#include "sched/sched_internal.h"
#include "sched/task.h"
#include "sched/sched_policy.h"
#include "sched/runqueue.h"
#include "sched/fpu.h"
#include "dynpriv/dynpriv.h"
#include "percpu/percpu.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "mm/kva.h"
#include "mm/paging.h"
#include "common/logging.h"
#include "sync/spinlock.h"
#include "smp/smp.h"
#include "hw/cpu.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "rc/reaper.h"
#include "exec/elf.h"
#include "mm/pmm.h"
#include "common/string.h"

DEFINE_PER_CPU(sched::task*, current_task);
DEFINE_PER_CPU(bool, percpu_is_elevated);
DEFINE_PER_CPU(uint32_t, percpu_cpu_id);
static DEFINE_PER_CPU(sched::task*, pending_off_cpu_task);
static DEFINE_PER_CPU(uint64_t, cpu_tlb_sync_epoch);

static DEFINE_PER_CPU(sched::runqueue, cpu_rq);

static uint32_t g_next_tid = 1;
static uint32_t g_pending_tlb_sync_tickets = 0;

__PRIVILEGED_DATA static uint32_t g_lb_next_cpu = 0;

namespace sched {

constexpr size_t TASK_STACK_PAGES = 4;
constexpr uint16_t TASK_GUARD_PAGES = 1;

constexpr size_t SYSTEM_STACK_PAGES = 4;
constexpr uint16_t SYSTEM_GUARD_PAGES = 1;

constexpr uintptr_t USER_STACK_TOP   = 0x7FFFFFF00000;
constexpr size_t    USER_STACK_PAGES = 8; // 32KB

constexpr uint64_t TLB_SYNC_CPU_IGNORED = ~0ULL;

static uint32_t load_cleanup_stage(const task* t) {
    return __atomic_load_n(&t->cleanup_stage, __ATOMIC_ACQUIRE);
}

static void store_cleanup_stage(task* t, uint32_t stage) {
    __atomic_store_n(&t->cleanup_stage, stage, __ATOMIC_RELEASE);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static rc::reaper::cleanup_result reap_task(sched::task* t) {
    uint32_t stage = load_cleanup_stage(t);
    if (stage < TASK_CLEANUP_STAGE_SCHEDULER_DETACHED) {
        return rc::reaper::RETRY_LATER;
    }

    if (__atomic_load_n(&t->exec.on_cpu, __ATOMIC_ACQUIRE)) {
        return rc::reaper::RETRY_LATER;
    }

    uint32_t cpu_count = smp::cpu_count();
    if (stage == TASK_CLEANUP_STAGE_SCHEDULER_DETACHED) {
        for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
            smp::cpu_info* info = smp::get_cpu_info(cpu);
            if (!info || __atomic_load_n(&info->state, __ATOMIC_ACQUIRE) != smp::CPU_ONLINE) {
                t->tlb_sync_ticket.cpu_epoch_snapshot[cpu] = TLB_SYNC_CPU_IGNORED;
                continue;
            }
            t->tlb_sync_ticket.cpu_epoch_snapshot[cpu] =
                __atomic_load_n(&per_cpu_on(cpu_tlb_sync_epoch, cpu), __ATOMIC_ACQUIRE);
        }
        __atomic_store_n(&t->tlb_sync_ticket.armed, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_pending_tlb_sync_tickets, 1, __ATOMIC_ACQ_REL);
        store_cleanup_stage(t, TASK_CLEANUP_STAGE_WAITING_FOR_TLB_SYNC);
        return rc::reaper::RETRY_LATER;
    }

    if (stage == TASK_CLEANUP_STAGE_WAITING_FOR_TLB_SYNC) {
        if (__atomic_load_n(&t->tlb_sync_ticket.armed, __ATOMIC_ACQUIRE) == 0) {
            return rc::reaper::RETRY_LATER;
        }

        for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
            if (t->tlb_sync_ticket.cpu_epoch_snapshot[cpu] == TLB_SYNC_CPU_IGNORED) {
                continue;
            }
            uint64_t epoch = __atomic_load_n(&per_cpu_on(cpu_tlb_sync_epoch, cpu), __ATOMIC_ACQUIRE);
            if ((epoch - t->tlb_sync_ticket.cpu_epoch_snapshot[cpu]) == 0) {
                return rc::reaper::RETRY_LATER;
            }
        }
        __atomic_sub_fetch(&g_pending_tlb_sync_tickets, 1, __ATOMIC_ACQ_REL);
        store_cleanup_stage(t, TASK_CLEANUP_STAGE_READY_TO_RECLAIM);
    }

    if (load_cleanup_stage(t) != TASK_CLEANUP_STAGE_READY_TO_RECLAIM) {
        return rc::reaper::RETRY_LATER;
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

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void finalize_pending_off_cpu() {
    task* pending = this_cpu(pending_off_cpu_task);
    if (!pending) {
        return;
    }

    this_cpu(pending_off_cpu_task) = nullptr;
    __atomic_store_n(&pending->exec.on_cpu, 0, __ATOMIC_RELEASE);
    cpu::send_event();

    if (load_cleanup_stage(pending) == TASK_CLEANUP_STAGE_SCHEDULER_DETACHED) {
        // The reaper must only start cleanup after off-CPU publication is visible.
        rc::reaper::defer(&pending->reaper_node);
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
    if (__atomic_load_n(&g_pending_tlb_sync_tickets, __ATOMIC_ACQUIRE) == 0) {
        return;
    }
    paging::flush_tlb_all();
    __atomic_add_fetch(&this_cpu(cpu_tlb_sync_epoch), 1, __ATOMIC_RELEASE);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint32_t load_balance_select_cpu() {
    uint32_t online = smp::online_count();
    if (online <= 1) return 0;

    uint32_t target = __atomic_fetch_add(&g_lb_next_cpu, 1, __ATOMIC_RELAXED) % online;
    uint32_t total = smp::cpu_count();
    uint32_t seen = 0;
    for (uint32_t i = 0; i < total; i++) {
        smp::cpu_info* info = smp::get_cpu_info(i);
        if (info && __atomic_load_n(&info->state, __ATOMIC_ACQUIRE) == smp::CPU_ONLINE) {
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
    runqueue& rq = this_cpu(cpu_rq);

    sync::irq_state irq = sync::spin_lock_irqsave(rq.lock);

    // Only re-enqueue if prev was running (not dead, blocked, or already woken)
    if (prev != rq.idle_task && prev->state == TASK_STATE_RUNNING) {
        prev->state = TASK_STATE_READY;
        rq.policy->enqueue(prev);
        rq.nr_running++;
    }

    // Dead task is now scheduler-detached and can enter deferred cleanup flow.
    if (prev != rq.idle_task && prev->state == TASK_STATE_DEAD) {
        store_cleanup_stage(prev, TASK_CLEANUP_STAGE_SCHEDULER_DETACHED);
    }

    task* next = rq.policy->pick_next();
    if (next) {
        rq.nr_running--;
    } else {
        next = rq.idle_task;
    }

    next->state = TASK_STATE_RUNNING;
    this_cpu(current_task) = next;
    this_cpu(current_task_exec) = &next->exec;
    this_cpu(percpu_is_elevated) = (next->exec.flags & TASK_FLAG_ELEVATED) != 0;

    sync::spin_unlock_irqrestore(rq.lock, irq);

    return next;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enqueue(task* t) {
    uint32_t expected = TASK_STATE_CREATED;
    if (!__atomic_compare_exchange_n(&t->state, &expected, TASK_STATE_READY,
                                      false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
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
    if (!__atomic_compare_exchange_n(&t->state, &expected, TASK_STATE_READY,
                                      false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
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
__PRIVILEGED_CODE void wake(task* t) {
    uint32_t expected = TASK_STATE_BLOCKED;
    if (!__atomic_compare_exchange_n(&t->state, &expected, TASK_STATE_READY,
                                      false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        return;
    }

    uint32_t task_cpu = __atomic_load_n(&t->exec.cpu, __ATOMIC_RELAXED);
    if (task_cpu != percpu::current_cpu_id()) {
        while (__atomic_load_n(&t->exec.on_cpu, __ATOMIC_ACQUIRE)) {
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
__PRIVILEGED_CODE void sleep_ns(uint64_t ns) {
    if (ns == 0) {
        yield();
        return;
    }

    task* self = current();
    if (self->exec.flags & TASK_FLAG_IDLE) {
        return;
    }

    uint64_t deadline = clock::now_ns() + ns;
    self->state = TASK_STATE_BLOCKED;
    timer::schedule_sleep(self, deadline);
    yield();
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
        store_cleanup_stage(task, TASK_CLEANUP_STAGE_EXIT_REQUESTED);
        task->state = TASK_STATE_DEAD;
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

    t->tid = __atomic_fetch_add(&g_next_tid, 1, __ATOMIC_RELAXED);
    t->state = TASK_STATE_CREATED;
    t->sched_link = {};
    t->wait_link = {};
    t->timer_link = {};
    t->timer_deadline = 0;
    t->name = name;
    t->cleanup_stage = TASK_CLEANUP_STAGE_ACTIVE;
    t->tlb_sync_ticket.armed = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        t->tlb_sync_ticket.cpu_epoch_snapshot[i] = 0;
    }
    fpu::init_state(&t->exec.fpu_ctx);
    t->reaper_node.init(reap_task_thunk);

    return t;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task* create_user_task(
    const exec::loaded_image& image, const char* name
) {
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

    // User stack: allocate physical pages and map into user page table
    uintptr_t user_stack_base = USER_STACK_TOP - USER_STACK_PAGES * pmm::PAGE_SIZE;
    for (size_t i = 0; i < USER_STACK_PAGES; i++) {
        pmm::phys_addr_t phys = pmm::alloc_page();
        if (phys == 0) {
            log::error("sched: failed to allocate user stack page");
            for (size_t j = 0; j < i; j++) {
                uintptr_t va = user_stack_base + j * pmm::PAGE_SIZE;
                pmm::phys_addr_t p = paging::get_physical(va, image.pt_root);
                paging::unmap_page(va, image.pt_root);
                if (p != 0) pmm::free_page(p);
            }
            vmm::free(sys_stack_base);
            heap::kfree_delete(t);
            return nullptr;
        }

        string::memset(paging::phys_to_virt(phys), 0, pmm::PAGE_SIZE);

        uintptr_t va = user_stack_base + i * pmm::PAGE_SIZE;
        int32_t rc = paging::map_page(va, phys, paging::PAGE_USER_RW, image.pt_root);
        if (rc != paging::OK) {
            log::error("sched: failed to map user stack page");
            pmm::free_page(phys);
            for (size_t j = 0; j < i; j++) {
                uintptr_t prev_va = user_stack_base + j * pmm::PAGE_SIZE;
                pmm::phys_addr_t p = paging::get_physical(prev_va, image.pt_root);
                paging::unmap_page(prev_va, image.pt_root);
                if (p != 0) {
                    pmm::free_page(p);
                }
            }
            vmm::free(sys_stack_base);
            heap::kfree_delete(t);
            return nullptr;
        }
    }

    t->exec.flags = TASK_FLAG_PREEMPTIBLE;
    t->exec.cpu = 0;
    t->exec.task_stack_top = USER_STACK_TOP;
    t->exec.system_stack_top = sys_stack_top;
    t->exec.pt_root = paging::supervisor_pt_root_for_user_task(image.pt_root);
    t->exec.user_pt_root = image.pt_root;
    t->exec.tls_base = 0;
    t->task_stack_base = 0; // user stack is not VMM-allocated
    t->sys_stack_base = sys_stack_base;

    uint8_t* ctx_bytes = reinterpret_cast<uint8_t*>(&t->exec.cpu_ctx);
    for (size_t i = 0; i < sizeof(thread_cpu_context); i++) {
        ctx_bytes[i] = 0;
    }
    auto entry = reinterpret_cast<void(*)(void*)>(image.entry_point);
    arch_init_task_context(t, entry, nullptr);

    t->exec.on_cpu = 0;

    t->tid = __atomic_fetch_add(&g_next_tid, 1, __ATOMIC_RELAXED);
    t->state = TASK_STATE_CREATED;
    t->sched_link = {};
    t->wait_link = {};
    t->timer_link = {};
    t->timer_deadline = 0;
    t->name = name;
    t->cleanup_stage = TASK_CLEANUP_STAGE_ACTIVE;
    t->tlb_sync_ticket.armed = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        t->tlb_sync_ticket.cpu_epoch_snapshot[i] = 0;
    }
    fpu::init_state(&t->exec.fpu_ctx);
    t->reaper_node.init(reap_task_thunk);

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
    idle->exec.flags |= TASK_FLAG_IDLE;
    idle->tid = 0;
    idle->state = TASK_STATE_RUNNING;
    idle->task_stack_base = 0;
    idle->sys_stack_base = 0;
    idle->sched_link = {};
    idle->wait_link = {};
    idle->timer_link = {};
    idle->timer_deadline = 0;
    idle->name = "idle";
    idle->cleanup_stage = TASK_CLEANUP_STAGE_ACTIVE;
    idle->tlb_sync_ticket.armed = 0;
    fpu::init_state(&idle->exec.fpu_ctx);

    this_cpu(current_task) = idle;
    this_cpu(current_task_exec) = &idle->exec;
    this_cpu(percpu_is_elevated) = (idle->exec.flags & TASK_FLAG_ELEVATED) != 0;
    this_cpu(pending_off_cpu_task) = nullptr;
    this_cpu(cpu_tlb_sync_epoch) = 0;

    // Initialize per-CPU runqueue
    runqueue& rq = this_cpu(cpu_rq);
    rq.lock = sync::SPINLOCK_INIT;
    rq.nr_running = 0;
    rq.idle_task = idle;

    // Allocate round-robin policy
    auto* policy = heap::kalloc_new<round_robin_policy>();
    if (!policy) {
        log::error("sched: failed to allocate scheduling policy");
        return ERR_NO_MEM;
    }
    policy->init();
    rq.policy = policy;

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

    auto* dst = reinterpret_cast<uint8_t*>(idle);
    for (size_t i = 0; i < sizeof(task); i++) {
        dst[i] = 0;
    }

    idle->exec.flags = TASK_FLAG_IDLE | TASK_FLAG_ELEVATED | TASK_FLAG_KERNEL
                     | TASK_FLAG_CAN_ELEVATE | TASK_FLAG_PREEMPTIBLE;
    idle->exec.cpu = cpu_id;
    idle->exec.on_cpu = 1;
    idle->exec.task_stack_top = task_stack_top;
    idle->exec.system_stack_top = system_stack_top;
    idle->exec.pt_root = paging::get_kernel_pt_root();
    idle->exec.user_pt_root = 0;
    idle->task_stack_base = 0;
    idle->sys_stack_base = 0;
    idle->tid = __atomic_fetch_add(&g_next_tid, 1, __ATOMIC_RELAXED);
    idle->state = TASK_STATE_RUNNING;
    idle->name = "idle";
    idle->cleanup_stage = TASK_CLEANUP_STAGE_ACTIVE;
    idle->tlb_sync_ticket.armed = 0;
    fpu::init_state(&idle->exec.fpu_ctx);

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
