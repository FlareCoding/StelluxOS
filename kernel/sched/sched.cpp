#include "sched/sched.h"
#include "sched/sched_internal.h"
#include "sched/task.h"
#include "sched/sched_policy.h"
#include "sched/runqueue.h"
#include "dynpriv/dynpriv.h"
#include "percpu/percpu.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "mm/kva.h"
#include "common/logging.h"
#include "sync/spinlock.h"

DEFINE_PER_CPU(sched::task*, current_task);
DEFINE_PER_CPU(bool, percpu_is_elevated);

static DEFINE_PER_CPU(sched::runqueue, cpu_rq);

static uint32_t g_next_tid = 1;

namespace sched {

constexpr size_t TASK_STACK_PAGES = 4;
constexpr uint16_t TASK_GUARD_PAGES = 1;

constexpr size_t SYSTEM_STACK_PAGES = 4;
constexpr uint16_t SYSTEM_GUARD_PAGES = 1;

task* current() {
    return this_cpu(current_task);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task* pick_next_and_switch(task* prev) {
    runqueue& rq = this_cpu(cpu_rq);

    sync::spin_lock(rq.lock);

    // Re-enqueue prev unless it's idle or dead
    if (prev != rq.idle_task && prev->state != TASK_STATE_DEAD) {
        prev->state = TASK_STATE_READY;
        rq.policy->enqueue(prev);
        rq.nr_running++;
    }

    task* next = rq.policy->pick_next();
    if (next) {
        rq.nr_running--;
    } else {
        next = rq.idle_task;
    }

    next->state = TASK_STATE_RUNNING;
    this_cpu(current_task) = next;

    sync::spin_unlock(rq.lock);

    return next;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enqueue(task* t) {
    runqueue& rq = this_cpu(cpu_rq);

    sync::spin_lock(rq.lock);
    t->state = TASK_STATE_READY;
    rq.policy->enqueue(t);
    rq.nr_running++;
    sync::spin_unlock(rq.lock);
}

[[noreturn]] void exit(int exit_code) {
    RUN_ELEVATED({
        sched::task* task = current();
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

    // Zero cpu_ctx, then set arch-specific initial state
    uint8_t* ctx_bytes = reinterpret_cast<uint8_t*>(&t->exec.cpu_ctx);
    for (size_t i = 0; i < sizeof(thread_cpu_context); i++) {
        ctx_bytes[i] = 0;
    }
    arch_init_task_context(t, entry, arg);

    t->tid = g_next_tid++;
    t->state = TASK_STATE_READY;
    t->sched_link = {};
    t->name = name;

    log::debug("sched: created task '%s' tid=%u stack=%p", name, t->tid,
               reinterpret_cast<void*>(task_stack_top));

    return t;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    // Adopt the current boot task as idle
    static task idle_task = {};

    // Copy exec core from the existing boot task (byte copy to avoid memcpy)
    task_exec_core* boot_exec = this_cpu(current_task_exec);
    if (boot_exec) {
        auto* dst = reinterpret_cast<uint8_t*>(&idle_task.exec);
        auto* src = reinterpret_cast<const uint8_t*>(boot_exec);
        for (size_t i = 0; i < sizeof(task_exec_core); i++) {
            dst[i] = src[i];
        }
    }
    idle_task.exec.flags |= TASK_FLAG_IDLE;
    idle_task.tid = 0;
    idle_task.state = TASK_STATE_RUNNING;
    idle_task.name = "idle";

    this_cpu(current_task) = &idle_task;
    this_cpu(current_task_exec) = &idle_task.exec;
    this_cpu(percpu_is_elevated) = (idle_task.exec.flags & TASK_FLAG_ELEVATED) != 0;

    // Initialize per-CPU runqueue
    runqueue& rq = this_cpu(cpu_rq);
    rq.lock = sync::SPINLOCK_INIT;
    rq.nr_running = 0;
    rq.idle_task = &idle_task;

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

} // namespace sched
