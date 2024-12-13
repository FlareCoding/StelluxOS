#include <process/process.h>
#include <memory/memory.h>

DEFINE_PER_CPU(task_control_block*, current_task);

namespace sched {
// Saves context registers from the interrupt frame into a CPU context struct
__PRIVILEGED_CODE
void save_cpu_context(ptregs* process_context, ptregs* irq_frame) {
    memcpy(process_context, irq_frame, sizeof(ptregs));
}

// Saves context registers from the CPU context struct into an interrupt frame
__PRIVILEGED_CODE
void restore_cpu_context(ptregs* process_context, ptregs* irq_frame) {
    memcpy(irq_frame, process_context, sizeof(ptregs));
}

// Saves and restores necessary registers into the appropriate
// process control blocks using an interrupt frame.
// *Note* Meant to be called from within an interrupt handler
// and context would get switched upon interrupt return.
__PRIVILEGED_CODE
void switch_context_in_irq(
    int old_cpu,
    int new_cpu,
    task_control_block* from,
    task_control_block* to,
    ptregs* irq_frame
) {
    __unused old_cpu;
    __unused new_cpu;

    // Save the current context into the 'from' PCB
    save_cpu_context(&from->cpu_context, irq_frame);

    // Restore the context from the 'to' PCB
    restore_cpu_context(&to->cpu_context, irq_frame);

    // Set the new value of current_task for the current CPU
    this_cpu_write(current_task, to);
}
} // namespace sched
