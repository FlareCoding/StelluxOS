#include "trap_frame.h"
#include "defs/exception.h"
#include "common/types.h"
#include "common/logging.h"
#include "debug/panic.h"
#include "sched/task_exec_core.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"
#include "irq/irq.h"
#include "irq/irq_arch.h"
#include "io/serial.h"
#include "hwtimer/hwtimer_arch.h"
#include "timer/timer.h"

// Forward declaration of syscall dispatch
extern "C" void stlx_aarch64_syscall_dispatch(aarch64::trap_frame* tf);

namespace arch {
__PRIVILEGED_CODE bool msi_handle_irq(uint32_t irq_id);
} // namespace arch

namespace sched {
__PRIVILEGED_CODE void on_tick(aarch64::trap_frame* tf);
} // namespace sched

// RAII helper to manage TASK_FLAG_IN_IRQ
struct irq_context_guard {
    sched::task_exec_core* task_core;
    irq_context_guard() : task_core(this_cpu(current_task_exec)) {
        task_core->flags |= sched::TASK_FLAG_IN_IRQ;
    }
    ~irq_context_guard() {
        task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
    }
};

[[noreturn]] __PRIVILEGED_CODE 
static void trap_fatal(const char* kind, const aarch64::trap_frame* tf) {
    panic::on_trap(const_cast<aarch64::trap_frame*>(tf), kind);
}

__PRIVILEGED_CODE static inline void restore_post_trap_elevation_state() {
    // Return-boundary restoration: select runtime elevation based on the
    // currently selected task's privilege-mode bit.
    this_cpu(percpu_is_elevated) =
        (this_cpu(current_task_exec)->flags & sched::TASK_FLAG_ELEVATED) != 0;
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el0_sync_handler(aarch64::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;
    irq_context_guard guard;
    
    const uint64_t esr = tf->esr;
    const uint8_t ec = static_cast<uint8_t>((esr >> aarch64::ESR_EC_SHIFT) & aarch64::ESR_EC_MASK);

    if (ec == aarch64::EC_SVC_A64) {
        stlx_aarch64_syscall_dispatch(tf);
        restore_post_trap_elevation_state();
        return;
    }

    trap_fatal("el0 sync", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el0_irq_handler(aarch64::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;

    sched::task_exec_core* irq_task_core = this_cpu(current_task_exec);
    irq_task_core->flags |= sched::TASK_FLAG_IN_IRQ;

    uint32_t irq_id = irq::acknowledge();
    if (irq_id == hwtimer::TIMER_PPI) {
        bool tick = timer::on_interrupt();
        irq::eoi(irq_id);
        if (tick) {
            sched::on_tick(tf);
        }
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    if (irq_id == serial::irq_id()) {
        serial::on_rx_irq();
        irq::eoi(irq_id);
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    if (arch::msi_handle_irq(irq_id)) {
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    if (irq_id != irq::GIC_SPURIOUS_ID) {
        irq::eoi(irq_id);
    }
    irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
    trap_fatal("el0 irq", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el0_fiq_handler(aarch64::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;
    irq_context_guard guard;
    trap_fatal("el0 fiq", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el0_serror_handler(aarch64::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;
    irq_context_guard guard;
    trap_fatal("el0 serror", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el1_sync_handler(aarch64::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;
    irq_context_guard guard;
    
    const uint64_t esr = tf->esr;
    const uint8_t ec = static_cast<uint8_t>((esr >> aarch64::ESR_EC_SHIFT) & aarch64::ESR_EC_MASK);

    if (ec == aarch64::EC_SVC_A64) {
        stlx_aarch64_syscall_dispatch(tf);
        restore_post_trap_elevation_state();
        return;
    }

    trap_fatal("el1 sync", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el1_irq_handler(aarch64::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;

    sched::task_exec_core* irq_task_core = this_cpu(current_task_exec);
    irq_task_core->flags |= sched::TASK_FLAG_IN_IRQ;

    uint32_t irq_id = irq::acknowledge();
    if (irq_id == hwtimer::TIMER_PPI) {
        bool tick = timer::on_interrupt();
        irq::eoi(irq_id);
        if (tick) {
            sched::on_tick(tf);
        }
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    if (irq_id == serial::irq_id()) {
        serial::on_rx_irq();
        irq::eoi(irq_id);
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    if (arch::msi_handle_irq(irq_id)) {
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    if (irq_id != irq::GIC_SPURIOUS_ID) {
        irq::eoi(irq_id);
    }
    irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
    trap_fatal("el1 irq", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el1_fiq_handler(aarch64::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;
    irq_context_guard guard;
    trap_fatal("el1 fiq", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el1_serror_handler(aarch64::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;
    irq_context_guard guard;
    trap_fatal("el1 serror", tf);
}
