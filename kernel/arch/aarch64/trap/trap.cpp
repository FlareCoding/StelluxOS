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
#include "serial/serial.h"
#include "hwtimer/hwtimer_arch.h"
#include "timer/timer.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
#include "mm/mm.h"

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
    constexpr uint32_t mask = sched::TASK_FLAG_ELEVATED | sched::TASK_FLAG_IN_SYSCALL;
    this_cpu(percpu_is_elevated) =
        (this_cpu(current_task_exec)->flags & mask) != 0;
}

static inline int ec_to_signal(uint8_t ec) {
    switch (ec) {
        case aarch64::EC_DATA_ABORT_LOWER:
        case aarch64::EC_INST_ABORT_LOWER:
        case aarch64::EC_SP_ALIGN:           return 11;  // SIGSEGV
        case aarch64::EC_UNKNOWN:            return 4;   // SIGILL
        case aarch64::EC_BRK_A64:            return 5;   // SIGTRAP
        case aarch64::EC_FP_A64:             return 8;   // SIGFPE
        case aarch64::EC_PC_ALIGN:           return 7;   // SIGBUS
        default:                             return 11;  // SIGSEGV fallback
    }
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
        restore_post_trap_elevation_state();
        return;
    }

    // If it's a page fault, attempt to handle it for on-demand paging
    if (in_user_code && (
        ec == aarch64::EC_DATA_ABORT_LOWER ||
        ec == aarch64::EC_INST_ABORT_LOWER)
    ) {
        uintptr_t fault_addr = aarch64::get_far(tf);
        uint32_t pf_flags = 0;

        // DFSC[5:0] is in ESR.ISS bits [5:0] for data/instruction aborts.
        uint32_t dfsc = esr & 0x3F;
        uint32_t fault_class = dfsc >> 2; // top 4 bits identify the class
        if (fault_class == 0b0011) pf_flags |= mm::PF_FLAG_PRESENT; // permission fault

        // ESR.ISS bit 6 = WnR (Write not Read) for data aborts.
        if (ec == aarch64::EC_DATA_ABORT_LOWER && (esr & (1u << 6))) {
            pf_flags |= mm::PF_FLAG_WRITE;
        }

        if (ec == aarch64::EC_INST_ABORT_LOWER) {
            pf_flags |= mm::PF_FLAG_INSTRUCTION;
        }

        if (mm::handle_user_pf(guard.task_core->mm_ctx, fault_addr, pf_flags)) {
            // Fault has been handled successfully, restart instruction
            restore_post_trap_elevation_state();
            return;
        }
    }

    if (in_user_code && (
        ec == aarch64::EC_DATA_ABORT_LOWER ||
        ec == aarch64::EC_INST_ABORT_LOWER ||
        ec == aarch64::EC_UNKNOWN          ||
        ec == aarch64::EC_PC_ALIGN         ||
        ec == aarch64::EC_SP_ALIGN         ||
        ec == aarch64::EC_FP_A64           ||
        ec == aarch64::EC_BRK_A64)
    ) {
        signals::die_from_signal(static_cast<uint32_t>(ec_to_signal(ec)));
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

    if (irq::dispatch(irq_id)) {
        irq::eoi(irq_id);
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

    if (irq::dispatch(irq_id)) {
        irq::eoi(irq_id);
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
