#include <sched/sched.h>
#include <interrupts/irq.h>
#include <serial/serial.h>

namespace sched {
DEFINE_INT_HANDLER(irq_handler_timer);

__PRIVILEGED_DATA
task_control_block g_idle_tasks[MAX_SYSTEM_CPUS];

__PRIVILEGED_CODE
task_control_block* get_idle_task(uint64_t cpu) {
    if (cpu > MAX_SYSTEM_CPUS - 1) {
        return nullptr;
    }

    return &g_idle_tasks[cpu];
}

__PRIVILEGED_CODE void install_sched_irq_handler() {
    // flags = 1 for fast apic EOI path on x86
    const uint8_t flags = 1;

    register_irq_handler(IRQ0, irq_handler_timer, flags, nullptr);
}

DEFINE_INT_HANDLER(irq_handler_timer) {
    __unused regs;
    __unused cookie;

    return IRQ_HANDLED;
}
} // namespace sched
