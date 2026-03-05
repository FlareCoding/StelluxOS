#include "timer/timer.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "clock/clock.h"
#include "hwtimer/hwtimer_arch.h"
#include "irq/irq.h"
#include "hw/cpu.h"
#include "percpu/percpu.h"
#include "sync/spinlock.h"
#include "common/list.h"
#include "common/logging.h"

namespace timer {

constexpr uint64_t NS_PER_SEC = 1000000000ULL;

struct timer_cpu_state {
    sync::spinlock lock;
    uint64_t tick_interval_ns;
    uint64_t next_tick_ns;
    uint64_t programmed_ns;
    uint32_t tick_interval_ticks;
    list::head<sched::task, &sched::task::timer_link> sleep_queue;
};

static DEFINE_PER_CPU(timer_cpu_state, cpu_timer_state);

__PRIVILEGED_BSS static uint64_t g_cnt_freq;
__PRIVILEGED_BSS static uint64_t g_inv_mult;
__PRIVILEGED_BSS static uint32_t g_inv_shift;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void compute_inv_mult_shift(uint64_t freq,
                                                      uint64_t* out_mult,
                                                      uint32_t* out_shift) {
    for (uint32_t s = 63; s > 0; s--) {
        if (freq > (0xFFFFFFFFFFFFFFFFULL >> s)) continue;
        uint64_t m = (freq << s) / NS_PER_SEC;
        if (m != 0) {
            *out_mult = m;
            *out_shift = s;
            return;
        }
    }
    *out_mult = freq / NS_PER_SEC;
    *out_shift = 0;
}

__PRIVILEGED_CODE static uint64_t ns_to_cnt_ticks(uint64_t ns) {
    return static_cast<uint64_t>(
        (static_cast<unsigned __int128>(ns) * g_inv_mult) >> g_inv_shift
    );
}

/**
 * Program using CNTV_TVAL (relative delta). Simpler and more reliable than
 * CVAL on QEMU TCG where absolute counter comparisons can be fragile.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void program_oneshot(uint64_t deadline_ns) {
    uint64_t now = clock::now_ns();
    uint64_t delta_ns = (deadline_ns > now) ? (deadline_ns - now) : 0;

    uint64_t delta_ticks = ns_to_cnt_ticks(delta_ns);
    if (delta_ticks == 0) delta_ticks = 1;
    if (delta_ticks > 0x7FFFFFFF) delta_ticks = 0x7FFFFFFF;

    hwtimer::write_cntv_tval(static_cast<uint32_t>(delta_ticks));
}

__PRIVILEGED_CODE static void program_next_event(timer_cpu_state& state) {
    uint64_t next_event = state.next_tick_ns;
    if (!state.sleep_queue.empty()) {
        uint64_t front_deadline = state.sleep_queue.front()->timer_deadline;
        if (front_deadline < next_event) {
            next_event = front_deadline;
        }
    }

    state.programmed_ns = next_event;
    program_oneshot(next_event);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init(uint32_t hz) {
    g_cnt_freq = hwtimer::read_cntfrq();
    if (g_cnt_freq == 0) {
        log::error("timer: CNTFRQ_EL0 is zero");
        return ERR;
    }

    compute_inv_mult_shift(g_cnt_freq, &g_inv_mult, &g_inv_shift);

    timer_cpu_state& state = this_cpu(cpu_timer_state);
    state.lock = sync::SPINLOCK_INIT;
    state.tick_interval_ns = NS_PER_SEC / hz;
    state.tick_interval_ticks = static_cast<uint32_t>(g_cnt_freq / hz);
    state.next_tick_ns = clock::now_ns() + state.tick_interval_ns;
    state.programmed_ns = state.next_tick_ns;
    state.sleep_queue.init();

    hwtimer::write_cntv_tval(state.tick_interval_ticks);
    hwtimer::write_cntv_ctl(1);
    irq::unmask(hwtimer::TIMER_PPI);
    cpu::irq_enable();

    log::info("timer: Generic Timer freq=%lu Hz, one-shot at %u Hz",
              g_cnt_freq, hz);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap(uint32_t hz) {
    uint64_t freq = hwtimer::read_cntfrq();
    if (freq == 0) {
        return ERR;
    }

    timer_cpu_state& state = this_cpu(cpu_timer_state);
    state.lock = sync::SPINLOCK_INIT;
    state.tick_interval_ns = NS_PER_SEC / hz;
    state.tick_interval_ticks = static_cast<uint32_t>(freq / hz);
    state.next_tick_ns = clock::now_ns() + state.tick_interval_ns;
    state.programmed_ns = state.next_tick_ns;
    state.sleep_queue.init();

    hwtimer::write_cntv_tval(state.tick_interval_ticks);
    hwtimer::write_cntv_ctl(1);
    irq::unmask(hwtimer::TIMER_PPI);
    cpu::irq_enable();

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void stop() {
    hwtimer::write_cntv_ctl(0);
    irq::mask(hwtimer::TIMER_PPI);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool on_interrupt() {
    timer_cpu_state& state = this_cpu(cpu_timer_state);
    sync::irq_state irq = sync::spin_lock_irqsave(state.lock);

    uint64_t now = clock::now_ns();

    if (now == 0) {
        hwtimer::write_cntv_tval(state.tick_interval_ticks);
        sync::spin_unlock_irqrestore(state.lock, irq);
        return true;
    }

    while (!state.sleep_queue.empty()) {
        sched::task* t = state.sleep_queue.front();
        if (t->timer_deadline > now) break;
        state.sleep_queue.pop_front();
        t->timer_deadline = 0;
        sched::wake(t);
    }

    bool tick_expired = (now >= state.next_tick_ns);
    if (tick_expired) {
        state.next_tick_ns += state.tick_interval_ns;
        if (state.next_tick_ns <= now) {
            state.next_tick_ns = now + state.tick_interval_ns;
        }
    }

    program_next_event(state);

    sync::spin_unlock_irqrestore(state.lock, irq);

    return tick_expired;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void schedule_sleep(sched::task* t, uint64_t deadline_ns) {
    timer_cpu_state& state = this_cpu(cpu_timer_state);
    sync::irq_state irq = sync::spin_lock_irqsave(state.lock);

    t->timer_deadline = deadline_ns;
    state.sleep_queue.insert_sorted(t,
        [](sched::task* a, sched::task* b) {
            return a->timer_deadline < b->timer_deadline;
        });

    if (deadline_ns < state.programmed_ns) {
        state.programmed_ns = deadline_ns;
        program_oneshot(deadline_ns);
    }

    sync::spin_unlock_irqrestore(state.lock, irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool cancel_sleep(sched::task* t) {
    if (!t) {
        return false;
    }

    uint32_t cpu_id = __atomic_load_n(&t->exec.cpu, __ATOMIC_ACQUIRE);
    timer_cpu_state& state = per_cpu_on(cpu_timer_state, cpu_id);
    sync::irq_state irq = sync::spin_lock_irqsave(state.lock);

    bool linked = t->timer_link.prev != nullptr && t->timer_link.next != nullptr;
    bool queued = t->timer_deadline != 0 && linked;
    if (!queued) {
        sync::spin_unlock_irqrestore(state.lock, irq);
        return false;
    }

    state.sleep_queue.remove(t);
    t->timer_deadline = 0;
    program_next_event(state);

    sync::spin_unlock_irqrestore(state.lock, irq);
    return true;
}

} // namespace timer
