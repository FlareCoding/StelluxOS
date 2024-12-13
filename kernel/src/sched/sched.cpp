#include <sched/sched.h>
#include <interrupts/irq.h>
#include <serial/serial.h>

#ifdef ARCH_X86_64
#include <arch/x86/apic/lapic.h>
#endif

namespace sched {
DEFINE_INT_HANDLER(irq_handler_timer);
DEFINE_INT_HANDLER(irq_handler_schedule);

__PRIVILEGED_DATA
task_control_block g_idle_tasks[MAX_SYSTEM_CPUS];

__PRIVILEGED_CODE
task_control_block* get_idle_task(uint64_t cpu) {
    if (cpu > MAX_SYSTEM_CPUS - 1) {
        return nullptr;
    }
    return &g_idle_tasks[cpu];
}

__PRIVILEGED_CODE
void install_sched_irq_handlers() {
    // flags = 1 for fast apic EOI path on x86
    const uint8_t flags = 1;
    register_irq_handler(IRQ0, irq_handler_timer, flags, nullptr);
    register_irq_handler(IRQ16, irq_handler_schedule, flags, nullptr);
}

DEFINE_INT_HANDLER(irq_handler_timer) {
    __unused cookie;
    __unused regs;
    //scheduler::get().__schedule(regs);

    return IRQ_HANDLED;
}

DEFINE_INT_HANDLER(irq_handler_schedule) {
    __unused cookie;
    __unused regs;
    //scheduler::get().__schedule(regs);

    return IRQ_HANDLED;
}

__PRIVILEGED_CODE
scheduler& scheduler::get() {
    GENERATE_STATIC_SINGLETON(scheduler);
}

__PRIVILEGED_CODE
void scheduler::init() {
    // Create the run queue for the main bsp cpu
    register_cpu_run_queue(BSP_CPU_ID);

    // Install the timer interrupt handler
    install_sched_irq_handlers();
}

__PRIVILEGED_CODE
void scheduler::register_cpu_run_queue(uint64_t cpu) {
    m_run_queues[cpu] = kstl::make_shared<sched_run_queue>();
}

__PRIVILEGED_CODE
void scheduler::add_task(task_control_block* task, int cpu) {
    // Mask the timer interrupts while adding the task to the queue
    preempt_disable(cpu);

    // Prepare the task
    task->cpu = cpu;
    task->state = process_state::READY;

    // Atomically add the task to the run-queue of the target processor
    m_run_queues[cpu]->add_task(task);

    // Unmask the timer interrupt and continue as usual
    preempt_enable(cpu);
}

__PRIVILEGED_CODE
void scheduler::remove_task(task_control_block* task) {
    int cpu = task->cpu;

    // Mask the timer interrupts while removing the task from the queue
    preempt_disable(cpu);

    // Atomically remove the task from the run-queue of the target processor
    m_run_queues[cpu]->remove_task(task);

    // Unmask the timer interrupt and continue as usual
    preempt_enable(cpu);
}

// Called from the IRQ interrupt context. Picks the
// next task to run and switches the context into it.
__PRIVILEGED_CODE
void scheduler::__schedule(ptregs* irq_frame) {
    int cpu = current->cpu;
    task_control_block* next = m_run_queues[cpu]->pick_next();
    if (next && next != current) {
        switch_context_in_irq(cpu, cpu, current, next, irq_frame);
    }
}

// Forces a new task to get scheduled and triggers a
// context switch without the need for a timer tick.
__PRIVILEGED_CODE
void scheduler::schedule() {
    asm volatile ("int $48");
}

// Masks timer tick-based interrupts
__PRIVILEGED_CODE
void scheduler::preempt_disable(int cpu) {
    if (cpu == -1) {
        cpu = current->cpu;
    }

#ifdef ARCH_X86_64
    arch::x86::lapic::get(cpu)->mask_timer_irq();
#endif
}

// Unmasks timer tick-based interrupts
__PRIVILEGED_CODE
void scheduler::preempt_enable(int cpu) {
        if (cpu == -1) {
        cpu = current->cpu;
    }

#ifdef ARCH_X86_64
    arch::x86::lapic::get(cpu)->unmask_timer_irq();
#endif
}
} // namespace sched
