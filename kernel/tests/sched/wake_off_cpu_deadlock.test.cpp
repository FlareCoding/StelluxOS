#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/sched_internal.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"
#include "hw/cpu.h"

using test_helpers::spin_wait;

TEST_SUITE(wake_off_cpu_deadlock);

// Reproduces the two-CPU off-CPU deadlock in sched::wake().
//
// Background: a task's exec.on_cpu is set to 1 when it is switched in and is
// only cleared lazily at the owning CPU's *next* scheduler trap (the switched
// out task is parked in the per-CPU pending_off_cpu_task and published by
// finalize_pending_off_cpu()). sched::wake() spin-waits on a remote task's
// on_cpu before enqueuing it.
//
// If two CPUs each hold a just-switched-out task in their pending slot (on_cpu
// still 1) and each tries to wake the task parked on the *other* CPU while
// running with interrupts disabled, neither CPU can reach a scheduler trap to
// clear the flag the other is spinning on -> permanent mutual stall.
//
// This test fabricates that exact precondition deterministically:
//   - Two controller tasks pin themselves to two distinct non-BSP CPUs with
//     interrupts disabled (so their CPUs cannot take a finalizing trap).
//   - Each parks a "victim" task in its own CPU's pending slot (on_cpu = 1,
//     state = BLOCKED) via sched::defer_off_cpu_finalize().
//   - After a barrier, each controller calls sched::wake() on the *other*
//     CPU's victim, entering the cross-CPU on_cpu spin.
//
// On buggy code both controllers spin forever; the BSP detects the stall via a
// bounded watchdog, fails the test, then recovers the wedged CPUs by clearing
// the victims' on_cpu so the suite can continue. On fixed code each wake()
// publishes its own pending task before spinning, so both make progress.

static constexpr uint32_t CTRL_A = 0;
static constexpr uint32_t CTRL_B = 1;

static volatile uint32_t g_dl_ready[2];
static volatile uint32_t g_dl_done[2];
static volatile uint32_t g_dl_create_failed;
static sched::task* volatile g_dl_victim[2];

static void dl_victim_fn(void*) {
    sched::exit(0);
}

static void dl_controller_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    uint32_t other = 1u - idx;

    RUN_ELEVATED({
        // Disable interrupts first: this both pins the controller to its CPU
        // and prevents any finalizing scheduler trap from clearing the victim's
        // on_cpu while the cross-wake is in flight.
        uint64_t flags = cpu::irq_save();
        uint32_t cpu = percpu::current_cpu_id();

        sched::task* victim = sched::create_kernel_task(
            dl_victim_fn, nullptr, "dl_victim");
        if (!victim) {
            __atomic_store_n(&g_dl_create_failed, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_dl_ready[idx], 1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_dl_done[idx], 1, __ATOMIC_RELEASE);
            cpu::irq_restore(flags);
        } else {
            // Fabricate the "parked off-CPU" state: the victim looks like a task
            // that was just switched out on this CPU and is awaiting on_cpu
            // publication at the next (never-arriving) trap.
            __atomic_store_n(&victim->exec.cpu, cpu, __ATOMIC_RELAXED);
            __atomic_store_n(&victim->exec.on_cpu, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&victim->state, sched::TASK_STATE_BLOCKED,
                __ATOMIC_RELEASE);
            __atomic_store_n(&g_dl_victim[idx], victim, __ATOMIC_RELEASE);

            // Install the victim as this CPU's deferred off-CPU task.
            sched::defer_off_cpu_finalize(victim);

            // Barrier: both pending slots must be established before any wake.
            __atomic_store_n(&g_dl_ready[idx], 1, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&g_dl_ready[other], __ATOMIC_ACQUIRE)) {
                cpu::relax();
            }

            // Cross-wake the task parked on the other CPU. This is the call that
            // deadlocks on buggy code.
            sched::task* target = __atomic_load_n(&g_dl_victim[other],
                __ATOMIC_ACQUIRE);
            if (target) {
                sched::wake(target);
            }

            __atomic_store_n(&g_dl_done[idx], 1, __ATOMIC_RELEASE);
            cpu::irq_restore(flags);
        }
    });

    sched::exit(0);
}

// --- mutual_cross_wake_makes_progress ---
// Proves: two CPUs cross-waking each other's parked-off-CPU task both make
// progress (no permanent spin in sched::wake()).

TEST(wake_off_cpu_deadlock, mutual_cross_wake_makes_progress) {
    uint32_t cpus = smp::cpu_count();
    // Needs two controller CPUs plus the BSP running this test body.
    if (cpus < 3) return;

    uint32_t self = percpu::current_cpu_id();

    // Pick two CPUs distinct from the one running this test.
    uint32_t ctrl_cpu[2];
    uint32_t picked = 0;
    for (uint32_t c = 0; c < cpus && picked < 2; c++) {
        if (c != self) {
            ctrl_cpu[picked++] = c;
        }
    }
    ASSERT_EQ(picked, 2u);

    g_dl_ready[0] = 0;
    g_dl_ready[1] = 0;
    g_dl_done[0] = 0;
    g_dl_done[1] = 0;
    g_dl_create_failed = 0;
    g_dl_victim[0] = nullptr;
    g_dl_victim[1] = nullptr;

    RUN_ELEVATED({
        sched::task* a = sched::create_kernel_task(
            dl_controller_fn,
            reinterpret_cast<void*>(static_cast<uintptr_t>(CTRL_A)),
            "dl_ctrl_a");
        sched::task* b = sched::create_kernel_task(
            dl_controller_fn,
            reinterpret_cast<void*>(static_cast<uintptr_t>(CTRL_B)),
            "dl_ctrl_b");
        ASSERT_NOT_NULL(a);
        ASSERT_NOT_NULL(b);
        sched::enqueue_on(a, ctrl_cpu[0]);
        sched::enqueue_on(b, ctrl_cpu[1]);
    });

    bool progressed = spin_wait(&g_dl_done[0]) && spin_wait(&g_dl_done[1]);

    if (!progressed) {
        // Deadlock detected: break the wedged controllers out of their on_cpu
        // spin so the wedged CPUs recover and the rest of the suite can run.
        sched::task* va = __atomic_load_n(&g_dl_victim[0], __ATOMIC_ACQUIRE);
        sched::task* vb = __atomic_load_n(&g_dl_victim[1], __ATOMIC_ACQUIRE);
        if (va) __atomic_store_n(&va->exec.on_cpu, 0, __ATOMIC_RELEASE);
        if (vb) __atomic_store_n(&vb->exec.on_cpu, 0, __ATOMIC_RELEASE);
        spin_wait(&g_dl_done[0]);
        spin_wait(&g_dl_done[1]);
    }

    EXPECT_FALSE(static_cast<bool>(
        __atomic_load_n(&g_dl_create_failed, __ATOMIC_ACQUIRE)));
    EXPECT_TRUE(progressed);
}
