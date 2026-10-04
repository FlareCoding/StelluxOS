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
#include "smp/ipi.h"
#include "serial/serial.h"
#include "hw/hwtimer.h"
#include "timer/timer.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
#include "signals/delivery.h"
#include "mm/mm.h"
#include "mm/uaccess.h"

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

__PRIVILEGED_CODE static inline void restore_post_trap_elevation_state(const aarch64::trap_frame* tf) {
    // Taken from the frame, since a task can resume inside kernel code that a fault entered
    this_cpu(percpu_is_elevated) = !aarch64::from_user(tf);
}

// Translates an abort's ESR into the access description the mm layer expects
static uint32_t abort_pf_flags(uint64_t esr, uint8_t ec) {
    uint32_t pf_flags = 0;

    if (aarch64::is_permission_fault(esr)) {
        pf_flags |= mm::PF_FLAG_PRESENT;
    }

    // ESR.ISS bit 6 = WnR (Write not Read) for data aborts
    bool data_abort = ec == aarch64::EC_DATA_ABORT_LOWER || ec == aarch64::EC_DATA_ABORT_SAME;
    if (data_abort && (esr & (1u << 6))) {
        pf_flags |= mm::PF_FLAG_WRITE;
    }

    if (ec == aarch64::EC_INST_ABORT_LOWER) {
        pf_flags |= mm::PF_FLAG_INSTRUCTION;
    }

    return pf_flags;
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el0_sync_handler(aarch64::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;
    irq_context_guard guard;
    
    // Detect if this is a userland task vs a kernel thread that might be running under lowered CPL
    uint8_t in_user_code = aarch64::from_user(tf) && !(guard.task_core->flags & sched::TASK_FLAG_KERNEL);

    const uint64_t esr = tf->esr;
    const uint8_t ec = static_cast<uint8_t>((esr >> aarch64::ESR_EC_SHIFT) & aarch64::ESR_EC_MASK);

    if (ec == aarch64::EC_SVC_A64) {
        stlx_aarch64_syscall_dispatch(tf);
        restore_post_trap_elevation_state(tf);
        return;
    }

    // Demand paging only resolves a missing translation, an alignment or
    // permission fault on a mapped page would otherwise retry forever
    if (in_user_code && (
        ec == aarch64::EC_DATA_ABORT_LOWER ||
        ec == aarch64::EC_INST_ABORT_LOWER) &&
        aarch64::is_translation_fault(esr)
    ) {
        uintptr_t fault_addr = aarch64::get_far(tf);
        uint32_t pf_flags = abort_pf_flags(esr, ec);

        if (mm::handle_user_pf(guard.task_core->mm_ctx, fault_addr, pf_flags) == mm::MM_CTX_OK) {
            // Fault has been handled successfully, restart instruction
            restore_post_trap_elevation_state(tf);
            return;
        }
    }

    if (in_user_code) {
        signals::die_from_signal(aarch64::signal_for_user_exception(esr));
    }

    trap_fatal("el0 sync", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el0_irq_handler(aarch64::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;

    sched::task_exec_core* irq_task_core = this_cpu(current_task_exec);
    irq_task_core->flags |= sched::TASK_FLAG_IN_IRQ;

    uint32_t ack = irq::acknowledge();
    uint32_t irq_id = ack & irq::GIC_INTID_MASK;
    if (irq_id == hwtimer::TIMER_PPI) {
        bool tick = timer::on_interrupt();
        irq::eoi(ack);
        if (tick) {
            sched::on_tick(tf);
        }
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state(tf);
        return;
    }

    if (irq_id == irq::IPI_SGI_INTID) {
        smp::ipi::dispatch();
        irq::eoi(ack);
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state(tf);
        return;
    }

    if (irq_id == serial::irq_id()) {
        serial::on_rx_irq();
        irq::eoi(ack);
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state(tf);
        return;
    }

    if (arch::msi_handle_irq(irq_id)) {
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state(tf);
        return;
    }

    if (irq::dispatch(irq_id)) {
        irq::eoi(ack);
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state(tf);
        return;
    }

    if (irq_id != irq::GIC_SPURIOUS_ID) {
        irq::eoi(ack);
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
        restore_post_trap_elevation_state(tf);
        return;
    }

    // A kernel data abort is recoverable only when raised by a user copy
    if (ec == aarch64::EC_DATA_ABORT_SAME) {
        bool can_sleep = (tf->spsr & aarch64::SPSR_IRQ_MASK) == 0;
        if (
            mm::uaccess::handle_kernel_fault(
                guard.task_core->mm_ctx,
                &tf->elr,
                aarch64::get_far(tf),
                abort_pf_flags(esr, ec),
                can_sleep
            )
        ) {
            restore_post_trap_elevation_state(tf);
            return;
        }
    }

    trap_fatal("el1 sync", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el1_irq_handler(aarch64::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;

    sched::task_exec_core* irq_task_core = this_cpu(current_task_exec);
    irq_task_core->flags |= sched::TASK_FLAG_IN_IRQ;

    uint32_t ack = irq::acknowledge();
    uint32_t irq_id = ack & irq::GIC_INTID_MASK;
    if (irq_id == hwtimer::TIMER_PPI) {
        bool tick = timer::on_interrupt();
        irq::eoi(ack);
        if (tick) {
            sched::on_tick(tf);
        }
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state(tf);
        return;
    }

    if (irq_id == irq::IPI_SGI_INTID) {
        smp::ipi::dispatch();
        irq::eoi(ack);
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state(tf);
        return;
    }

    if (irq_id == serial::irq_id()) {
        serial::on_rx_irq();
        irq::eoi(ack);
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state(tf);
        return;
    }

    if (arch::msi_handle_irq(irq_id)) {
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state(tf);
        return;
    }

    if (irq::dispatch(irq_id)) {
        irq::eoi(ack);
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state(tf);
        return;
    }

    if (irq_id != irq::GIC_SPURIOUS_ID) {
        irq::eoi(ack);
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
