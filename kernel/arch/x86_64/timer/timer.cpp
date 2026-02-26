#include "timer/timer.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "clock/clock.h"
#include "irq/irq.h"
#include "irq/irq_arch.h"
#include "defs/vectors.h"
#include "hw/portio.h"
#include "hw/mmio.h"
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
    list::head<sched::task, &sched::task::timer_link> sleep_queue;
};

static DEFINE_PER_CPU(timer_cpu_state, cpu_timer_state);

__PRIVILEGED_BSS static uint64_t g_lapic_freq;
__PRIVILEGED_BSS static uint64_t g_inv_mult;
__PRIVILEGED_BSS static uint32_t g_inv_shift;

// PIT constants (same as former hwtimer calibration)
constexpr uint16_t PIT_CTRL     = 0x43;
constexpr uint16_t PIT_CH2_DATA = 0x42;
constexpr uint16_t PORT_B       = 0x61;
constexpr uint8_t  PORT_B_GATE  = 0x01;
constexpr uint8_t  PORT_B_SPKR  = 0x02;
constexpr uint8_t  PORT_B_OUT2  = 0x20;
constexpr uint16_t PIT_10MS     = 11932;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint64_t calibrate_lapic() {
    uintptr_t lapic = irq::get_lapic_va();

    uint32_t lvt = mmio::read32(lapic + irq::LAPIC_LVT_TIMER);
    mmio::write32(lapic + irq::LAPIC_LVT_TIMER, lvt | irq::LVT_MASKED);
    mmio::write32(lapic + irq::LAPIC_TIMER_DCR, 0x7);

    uint8_t port_b_orig = portio::in8(PORT_B);
    portio::out8(PORT_B, (port_b_orig & ~PORT_B_SPKR) & ~PORT_B_GATE);
    portio::out8(PIT_CTRL, 0xB0);
    portio::out8(PIT_CH2_DATA, PIT_10MS & 0xFF);
    portio::out8(PIT_CH2_DATA, PIT_10MS >> 8);

    portio::out8(PORT_B, (port_b_orig & ~PORT_B_SPKR) | PORT_B_GATE);
    mmio::write32(lapic + irq::LAPIC_TIMER_ICR, 0xFFFFFFFF);

    while ((portio::in8(PORT_B) & PORT_B_OUT2) == 0) {
        cpu::relax();
    }

    uint32_t current = mmio::read32(lapic + irq::LAPIC_TIMER_CCR);
    uint32_t elapsed = 0xFFFFFFFF - current;
    mmio::write32(lapic + irq::LAPIC_TIMER_ICR, 0);
    portio::out8(PORT_B, port_b_orig);

    return static_cast<uint64_t>(elapsed) * 100;
}

/**
 * Precompute inv_mult/inv_shift for ns-to-ticks: ticks = (ns * inv_mult) >> inv_shift
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

__PRIVILEGED_CODE static uint64_t ns_to_lapic_ticks(uint64_t ns) {
    return static_cast<uint64_t>(
        (static_cast<unsigned __int128>(ns) * g_inv_mult) >> g_inv_shift
    );
}

/**
 * Program LAPIC one-shot timer for the given absolute deadline.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void program_oneshot(uint64_t deadline_ns) {
    uint64_t now = clock::now_ns();
    uint64_t delta_ns = (deadline_ns > now) ? (deadline_ns - now) : 0;

    uint64_t ticks = ns_to_lapic_ticks(delta_ns);
    uint32_t count = (ticks > 0xFFFFFFFF) ? 0xFFFFFFFF : static_cast<uint32_t>(ticks);
    if (count == 0) count = 1;

    uintptr_t lapic = irq::get_lapic_va();
    mmio::write32(lapic + irq::LAPIC_TIMER_ICR, count);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init(uint32_t hz) {
    uintptr_t lapic = irq::get_lapic_va();
    if (!lapic) {
        log::error("timer: LAPIC not initialized");
        return ERR;
    }

    g_lapic_freq = calibrate_lapic();
    if (g_lapic_freq == 0) {
        log::error("timer: LAPIC calibration failed");
        return ERR;
    }

    compute_inv_mult_shift(g_lapic_freq, &g_inv_mult, &g_inv_shift);

    // Configure one-shot mode (no LVT_PERIODIC), masked during setup
    mmio::write32(lapic + irq::LAPIC_LVT_TIMER,
                  irq::LVT_MASKED | x86::VEC_TIMER);
    mmio::write32(lapic + irq::LAPIC_TIMER_DCR, 0x7);

    // Initialize per-CPU timer state
    timer_cpu_state& state = this_cpu(cpu_timer_state);
    state.lock = sync::SPINLOCK_INIT;
    state.tick_interval_ns = NS_PER_SEC / hz;
    state.next_tick_ns = clock::now_ns() + state.tick_interval_ns;
    state.programmed_ns = state.next_tick_ns;
    state.sleep_queue.init();

    // Program first tick and unmask
    program_oneshot(state.next_tick_ns);
    mmio::write32(lapic + irq::LAPIC_LVT_TIMER, x86::VEC_TIMER);

    cpu::irq_enable();

    log::info("timer: LAPIC freq=%lu Hz, one-shot at %u Hz", g_lapic_freq, hz);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap(uint32_t hz) {
    uintptr_t lapic = irq::get_lapic_va();
    if (!lapic || g_lapic_freq == 0) {
        return ERR;
    }

    mmio::write32(lapic + irq::LAPIC_LVT_TIMER,
                  irq::LVT_MASKED | x86::VEC_TIMER);
    mmio::write32(lapic + irq::LAPIC_TIMER_DCR, 0x7);

    timer_cpu_state& state = this_cpu(cpu_timer_state);
    state.lock = sync::SPINLOCK_INIT;
    state.tick_interval_ns = NS_PER_SEC / hz;
    state.next_tick_ns = clock::now_ns() + state.tick_interval_ns;
    state.programmed_ns = state.next_tick_ns;
    state.sleep_queue.init();

    program_oneshot(state.next_tick_ns);
    mmio::write32(lapic + irq::LAPIC_LVT_TIMER, x86::VEC_TIMER);

    cpu::irq_enable();

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void stop() {
    uintptr_t lapic = irq::get_lapic_va();
    if (!lapic) return;

    uint32_t lvt = mmio::read32(lapic + irq::LAPIC_LVT_TIMER);
    mmio::write32(lapic + irq::LAPIC_LVT_TIMER, lvt | irq::LVT_MASKED);
    mmio::write32(lapic + irq::LAPIC_TIMER_ICR, 0);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool on_interrupt() {
    timer_cpu_state& state = this_cpu(cpu_timer_state);
    sync::irq_state irq = sync::spin_lock_irqsave(state.lock);

    uint64_t now = clock::now_ns();

    if (now == 0) {
        uint64_t ticks = ns_to_lapic_ticks(state.tick_interval_ns);
        uint32_t count = (ticks > 0xFFFFFFFF) ? 0xFFFFFFFF
                       : (ticks == 0)         ? 1
                       : static_cast<uint32_t>(ticks);
        uintptr_t lapic = irq::get_lapic_va();
        mmio::write32(lapic + irq::LAPIC_TIMER_ICR, count);
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

    uint64_t next_event = state.next_tick_ns;
    if (!state.sleep_queue.empty()) {
        uint64_t front_deadline = state.sleep_queue.front()->timer_deadline;
        if (front_deadline < next_event) {
            next_event = front_deadline;
        }
    }

    state.programmed_ns = next_event;
    program_oneshot(next_event);

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

} // namespace timer
