#include "trap_frame.h"
#include "defs/exception.h"
#include "common/types.h"
#include "common/logging.h"
#include "sched/task_exec_core.h"
#include "percpu/percpu.h"

// Forward declaration of syscall dispatch
extern "C" void stlx_aarch64_syscall_dispatch(aarch64::trap_frame* tf);

// RAII helper to manage TASK_FLAG_IN_IRQ
struct irq_context_guard {
    sched::task_exec_core* task;
    irq_context_guard() : task(this_cpu(current_task)) {
        task->flags |= sched::TASK_FLAG_IN_IRQ;
    }
    ~irq_context_guard() {
        task->flags &= ~sched::TASK_FLAG_IN_IRQ;
    }
};

[[noreturn]] __PRIVILEGED_CODE 
static void trap_fatal(const char* kind, const aarch64::trap_frame* tf) {
    const uint64_t esr = tf->esr;
    const unsigned int ec = static_cast<unsigned int>((esr >> aarch64::ESR_EC_SHIFT) & aarch64::ESR_EC_MASK);
    const unsigned int iss = static_cast<unsigned int>(esr & aarch64::ESR_ISS_MASK);

    log::fatal(
        "aarch64 trap (%s): from_user=%u elr=%p spsr=0x%lx esr=0x%lx (ec=0x%02x iss=0x%06x) far=%p sp=%p",
        kind,
        aarch64::from_user(tf) ? 1 : 0,
        reinterpret_cast<void*>(tf->elr),
        tf->spsr,
        tf->esr,
        ec,
        iss,
        reinterpret_cast<void*>(tf->far),
        reinterpret_cast<void*>(tf->sp));
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el0_sync_handler(aarch64::trap_frame* tf) {
    irq_context_guard guard;
    
    const uint64_t esr = tf->esr;
    const uint8_t ec = static_cast<uint8_t>((esr >> aarch64::ESR_EC_SHIFT) & aarch64::ESR_EC_MASK);

    // Check if this is an SVC from AArch64
    if (ec == aarch64::EC_SVC_A64) {
        stlx_aarch64_syscall_dispatch(tf);
        return;  // Return to continue execution, not fatal
    }

    trap_fatal("el0 sync", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el0_irq_handler(aarch64::trap_frame* tf) {
    irq_context_guard guard;
    trap_fatal("el0 irq", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el0_fiq_handler(aarch64::trap_frame* tf) {
    irq_context_guard guard;
    trap_fatal("el0 fiq", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el0_serror_handler(aarch64::trap_frame* tf) {
    irq_context_guard guard;
    trap_fatal("el0 serror", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el1_sync_handler(aarch64::trap_frame* tf) {
    irq_context_guard guard;
    
    const uint64_t esr = tf->esr;
    const uint8_t ec = static_cast<uint8_t>((esr >> aarch64::ESR_EC_SHIFT) & aarch64::ESR_EC_MASK);

    // Check if this is an SVC from AArch64 (e.g., elevate() called while already in EL1)
    if (ec == aarch64::EC_SVC_A64) {
        stlx_aarch64_syscall_dispatch(tf);
        return;  // Return to continue execution
    }

    trap_fatal("el1 sync", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el1_irq_handler(aarch64::trap_frame* tf) {
    irq_context_guard guard;
    trap_fatal("el1 irq", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el1_fiq_handler(aarch64::trap_frame* tf) {
    irq_context_guard guard;
    trap_fatal("el1 fiq", tf);
}

extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_el1_serror_handler(aarch64::trap_frame* tf) {
    irq_context_guard guard;
    trap_fatal("el1 serror", tf);
}
