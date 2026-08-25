#include "trap/trap_frame.h"
#include "defs/vectors.h"
#include "irq/irq.h"
#include "serial/serial.h"
#include "timer/timer.h"
#include "debug/panic.h"
#include "sched/task_exec_core.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"
#include "msi/msi.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
#include "mm/mm.h"

namespace sched {
__PRIVILEGED_CODE void on_yield(x86::trap_frame* tf);
__PRIVILEGED_CODE void on_tick(x86::trap_frame* tf);
} // namespace sched

__PRIVILEGED_CODE static inline void restore_post_trap_elevation_state() {
    // Return-boundary restoration: select runtime elevation based on the
    // currently selected task's privilege-mode bit.
    constexpr uint32_t mask = sched::TASK_FLAG_ELEVATED | sched::TASK_FLAG_IN_SYSCALL;
    this_cpu(percpu_is_elevated) =
        (this_cpu(current_task_exec)->flags & mask) != 0;
}

static inline int vector_to_signal(uint64_t vec) {
    switch (vec) {
        case x86::EXC_PAGE_FAULT:
        case x86::EXC_GENERAL_PROTECTION:
        case x86::EXC_STACK_FAULT:
        case x86::EXC_BOUND_RANGE:        return 11;  // SIGSEGV
        case x86::EXC_INVALID_OPCODE:     return 4;   // SIGILL
        case x86::EXC_DIVIDE_ERROR:
        case x86::EXC_OVERFLOW:
        case x86::EXC_X87_FPU:
        case x86::EXC_SIMD_FP:            return 8; // SIGFPE
        case x86::EXC_DEBUG:
        case x86::EXC_BREAKPOINT:         return 5;   // SIGTRAP
        case x86::EXC_ALIGNMENT_CHECK:    return 7;   // SIGBUS
        default:                          return 11;  // SIGSEGV fallback
    }
}

/**
 * @brief x86_64 trap handler called from assembly.
 * @note Privilege: **required**
 */
extern "C" __PRIVILEGED_CODE void stlx_x86_64_trap_handler(x86::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;

    sched::task_exec_core* irq_task_core = this_cpu(current_task_exec);
    
    // Detect if this is a userland task vs a kernel thread that might be running under lowered CPL
    uint8_t in_user_code = x86::from_user(tf) && !(irq_task_core->flags & sched::TASK_FLAG_KERNEL);

    // Mark as in interrupt context
    irq_task_core->flags |= sched::TASK_FLAG_IN_IRQ;

    if (tf->vector == x86::VEC_SCHED_YIELD) {
        sched::on_yield(tf);
        // Clear the IRQ flag on the originally interrupted task, not the post-switch task.
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    if (tf->vector == x86::VEC_TIMER) {
        irq::eoi(0);
        bool tick = timer::on_interrupt();
        if (tick) {
            sched::on_tick(tf);
        }
        // Clear IRQ state on the interrupted task to avoid stale IN_IRQ ownership.
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    if (tf->vector == x86::VEC_SERIAL) {
        irq::eoi(0);
        serial::on_rx_irq();
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    if (tf->vector >= x86::VEC_MSI_BASE &&
        tf->vector < x86::VEC_MSI_BASE + msi::capacity()) {
        irq::eoi(0);
        msi::dispatch(static_cast<uint32_t>(tf->vector - x86::VEC_MSI_BASE));
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    // If it's a page fault, attempt to handle it for on-demand paging
    if (in_user_code && tf->vector == x86::EXC_PAGE_FAULT) {
        uintptr_t fault_addr = x86::read_cr2();
        uint64_t ec = tf->error_code;
        uint32_t pf_flags = 0;
        if (ec & 0x1)  pf_flags |= mm::PF_FLAG_PRESENT;
        if (ec & 0x2)  pf_flags |= mm::PF_FLAG_WRITE;
        if (ec & 0x10) pf_flags |= mm::PF_FLAG_INSTRUCTION;

        if (mm::handle_user_pf(irq_task_core->mm_ctx, fault_addr, pf_flags)) {
            // Fault has been handled successfully, restart instruction
            irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
            restore_post_trap_elevation_state();
            return;
        }
    }

    if (in_user_code && (
        tf->vector == x86::EXC_PAGE_FAULT ||
        tf->vector == x86::EXC_GENERAL_PROTECTION ||
        tf->vector == x86::EXC_INVALID_OPCODE || 
        tf->vector == x86::EXC_DIVIDE_ERROR ||
        tf->vector == x86::EXC_OVERFLOW ||
        tf->vector == x86::EXC_BOUND_RANGE ||
        tf->vector == x86::EXC_STACK_FAULT ||
        tf->vector == x86::EXC_ALIGNMENT_CHECK ||
        tf->vector == x86::EXC_DEBUG ||
        tf->vector == x86::EXC_BREAKPOINT ||
        tf->vector == x86::EXC_X87_FPU ||
        tf->vector == x86::EXC_SIMD_FP)
    ) {
        signals::die_from_signal(static_cast<uint32_t>(vector_to_signal(tf->vector)));
    }

    panic::on_trap(tf);
}
