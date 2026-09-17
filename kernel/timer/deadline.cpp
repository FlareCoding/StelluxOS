#include "timer/timer.h"
#include "timer/timer_internal.h"
#include "clock/clock.h"
#include "hw/cpu.h"
#include "percpu/percpu.h"
#include "sched/sched.h"
#include "smp/smp.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "dynpriv/dynpriv.h"
#include "common/string.h"
#include "common/logging.h"

namespace timer {

// Earlier deadline first, then the order of scheduling among equal deadlines
struct deadline_order {
    bool operator()(const deadline_timer& a, const deadline_timer& b) const {
        if (a.deadline_ns != b.deadline_ns) {
            return a.deadline_ns < b.deadline_ns;
        }

        return a.sequence < b.sequence;
    }
};

using deadline_tree = rbt::tree<deadline_timer, &deadline_timer::link, deadline_order>;

struct deadline_cpu_state {
    sync::spinlock   lock;
    deadline_tree    tree;
    uint64_t         next_sequence;
    deadline_timer*  running;
    sync::wait_queue worker_wq;
};

// The timers due at a moment. Ones a callback schedules while the batch runs
// belong to the next batch, so no callback can keep a batch from ending.
struct expiry_batch {
    uint64_t now_ns;
    uint64_t sequence_limit;
};

// A timer carries this CPU number while it moves between trees, so anyone
// trying to lock its owner waits until the move is complete
constexpr uint32_t CPU_MIGRATING = ~0u;

static DEFINE_PER_CPU(deadline_cpu_state, cpu_deadline_state);

static bool has_state(const deadline_timer* timer, deadline_state value) {
    return timer->state.load_acquire() == static_cast<uint8_t>(value);
}

static void set_state(deadline_timer* timer, deadline_state value) {
    timer->state.store_release(static_cast<uint8_t>(value));
}

static deadline_timer* next_entry(deadline_timer* timer) {
    return rbt::node_to_entry<deadline_timer, &deadline_timer::link>(rbt::next(&timer->link));
}

/**
 * Locks the CPU state that owns `timer`, retrying while another CPU is moving
 * it. Returns with interrupts off and the owner's lock held.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static deadline_cpu_state& lock_owner(deadline_timer* timer, sync::irq_state* irq) {
    while (true) {
        uint32_t cpu_id = timer->cpu.load_acquire();
        if (cpu_id == CPU_MIGRATING) {
            cpu::relax();
            continue;
        }

        deadline_cpu_state& owner = per_cpu_on(cpu_deadline_state, cpu_id);
        *irq = sync::spin_lock_irqsave(owner.lock);

        if (timer->cpu.load_acquire() == cpu_id) {
            return owner;
        }

        sync::spin_unlock_irqrestore(owner.lock, *irq);
    }
}

static deadline_timer* first_due_locked(deadline_cpu_state& state, uint64_t now_ns, uint64_t pass_sequence) {
    for (deadline_timer* cur = state.tree.min(); cur && cur->deadline_ns <= now_ns; cur = next_entry(cur)) {
        if (cur->sequence < pass_sequence) {
            return cur;
        }
    }

    return nullptr;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static expiry_batch begin_batch(uint64_t now_ns) {
    sync::irq_state irq{cpu::irq_save()};
    deadline_cpu_state& state = this_cpu(cpu_deadline_state);
    sync::spin_lock(state.lock);

    expiry_batch batch = { now_ns, state.next_sequence };

    sync::spin_unlock_irqrestore(state.lock, irq);
    return batch;
}

/**
 * Removes the next timer due in the batch and records it as the one running.
 * Nullptr ends the batch.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static deadline_timer* take_next_due(const expiry_batch& batch) {
    sync::irq_state irq{cpu::irq_save()};
    deadline_cpu_state& state = this_cpu(cpu_deadline_state);
    sync::spin_lock(state.lock);

    deadline_timer* due = first_due_locked(state, batch.now_ns, batch.sequence_limit);
    if (due) {
        state.tree.remove(*due);
        set_state(due, deadline_state::idle);
        state.running = due;
    }

    sync::spin_unlock_irqrestore(state.lock, irq);
    return due;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void finish_callback() {
    sync::irq_state irq{cpu::irq_save()};
    deadline_cpu_state& state = this_cpu(cpu_deadline_state);
    sync::spin_lock(state.lock);

    state.running = nullptr;

    sync::spin_unlock_irqrestore(state.lock, irq);
}

/**
 * Sleeps until this CPU has a timer due. The interrupt wakes the sleeper at
 * every deadline it programs, so the wait ends on time without polling.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void wait_for_due() {
    sync::irq_state irq{cpu::irq_save()};
    deadline_cpu_state& state = this_cpu(cpu_deadline_state);
    sync::spin_lock(state.lock);

    while (true) {
        deadline_timer* first = state.tree.min();
        if (first && first->deadline_ns <= clock::now_ns()) {
            break;
        }

        irq = sync::wait(state.worker_wq, state.lock, irq);
    }

    sync::spin_unlock_irqrestore(state.lock, irq);
}

// Runs callbacks lowered, like a driver task, and elevates only around the
// tree. A faulty callback then faults instead of corrupting the kernel.
static void worker_entry(void*) {
    while (true) {
        RUN_ELEVATED(wait_for_due());

        expiry_batch batch = {};
        RUN_ELEVATED(batch = begin_batch(clock::now_ns()));

        while (true) {
            deadline_timer* due = nullptr;
            RUN_ELEVATED(due = take_next_due(batch));
            if (!due) {
                break;
            }

            due->fn(due);
            RUN_ELEVATED(finish_callback());
        }
    }

    sched::exit(0);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void deadline_init_this_cpu() {
    deadline_cpu_state& state = this_cpu(cpu_deadline_state);
    state.lock = sync::SPINLOCK_INIT;
    state.worker_wq.init();
}

/**
 * The due timers are the worker's to run, so the returned deadline skips
 * them, or the hardware would fire again at once for work already handed over.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint64_t deadline_interrupt(uint64_t now_ns) {
    deadline_cpu_state& state = this_cpu(cpu_deadline_state);
    bool due = false;
    uint64_t next = NO_DEADLINE;

    {
        sync::irq_lock_guard guard(state.lock);

        deadline_timer* cur = state.tree.min();
        while (cur && cur->deadline_ns <= now_ns) {
            due = true;
            cur = next_entry(cur);
        }

        if (cur) {
            next = cur->deadline_ns;
        }
    }

    if (due) {
        sync::wake_all(state.worker_wq);
    }

    return next;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void schedule(deadline_timer* timer, uint64_t deadline_ns) {
    if (!timer || !timer->fn) {
        return;
    }

    sync::irq_state irq;
    deadline_cpu_state* owner = &lock_owner(timer, &irq);

    if (has_state(timer, deadline_state::scheduled)) {
        owner->tree.remove(*timer);
        set_state(timer, deadline_state::idle);
    }

    // Interrupts are off, so this CPU cannot change under us. A timer owned
    // elsewhere is marked as moving while no lock is held, and anyone racing
    // for it waits in lock_owner until it has a tree again.
    uint32_t cpu_id = percpu::current_cpu_id();
    deadline_cpu_state& state = this_cpu(cpu_deadline_state);

    if (owner != &state) {
        timer->cpu.store_release(CPU_MIGRATING);
        sync::spin_unlock(owner->lock);
        sync::spin_lock(state.lock);
    }

    timer->deadline_ns = deadline_ns;
    timer->sequence = state.next_sequence++;
    timer->cpu.store_release(cpu_id);
    (void)state.tree.insert(timer);
    set_state(timer, deadline_state::scheduled);

    sync::spin_unlock_irqrestore(state.lock, irq);

    arch_request_deadline(deadline_ns);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool cancel(deadline_timer* timer) {
    if (!timer) {
        return true;
    }

    sync::irq_state irq;
    deadline_cpu_state& owner = lock_owner(timer, &irq);

    bool running = owner.running == timer;
    if (!running && has_state(timer, deadline_state::scheduled)) {
        owner.tree.remove(*timer);
        set_state(timer, deadline_state::idle);
    }

    sync::spin_unlock_irqrestore(owner.lock, irq);
    return !running;
}

bool is_pending(const deadline_timer* timer) {
    return timer && has_state(timer, deadline_state::scheduled);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void __dbg_test_fire_expired(uint64_t now_ns) {
    expiry_batch batch = begin_batch(now_ns);

    while (deadline_timer* due = take_next_due(batch)) {
        due->fn(due);
        finish_callback();
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t start_deadline_workers() {
    constexpr size_t PREFIX_LEN = 6;
    uint32_t total = smp::cpu_count();

    for (uint32_t cpu = 0; cpu < total; cpu++) {
        smp::cpu_info* info = smp::get_cpu_info(cpu);
        if (!info || info->state.load_acquire() != smp::CPU_ONLINE) {
            continue;
        }

        char name[sched::TASK_NAME_MAX] = "timerd";
        size_t len = PREFIX_LEN + string::format_u64(name + PREFIX_LEN, sizeof(name) - PREFIX_LEN - 1, cpu);
        name[len] = '\0';

        sched::task* worker = sched::create_kernel_task(worker_entry, nullptr, name);
        if (!worker) {
            log::error("timer: failed to create %s", name);
            return ERR;
        }

        sched::enqueue_on(worker, cpu);
    }

    return OK;
}

} // namespace timer
