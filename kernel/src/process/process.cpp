#include <process/process.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <arch/x86/gdt/gdt.h>

DEFINE_PER_CPU(task_control_block*, current_task);

#define SCHED_STACK_TOP_PADDING     0x80

#define SCHED_SYSTEM_STACK_PAGES    2
#define SCHED_TASK_STACK_PAGES      2

#define SCHED_SYSTEM_STACK_SIZE     SCHED_SYSTEM_STACK_PAGES * PAGE_SIZE - SCHED_STACK_TOP_PADDING
#define SCHED_TASK_STACK_SIZE       SCHED_TASK_STACK_PAGES * PAGE_SIZE - SCHED_STACK_TOP_PADDING

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

__PRIVILEGED_CODE
task_control_block* create_kernel_task(task_entry_fn_t entry, void* task_data) {
    task_control_block* task = new task_control_block();
    if (!task) {
        return nullptr;
    }

    // Initialize the task's process control block
    task->state = process_state::READY;
    task->pid = 0;
    task->elevated = 1;

    // Allocate both task and system stacks
    void* task_stack = vmm::alloc_contiguous_virtual_pages(SCHED_TASK_STACK_PAGES, DEFAULT_MAPPING_FLAGS);
    if (!task_stack) {
        delete task;
        return nullptr;
    }

    void* system_stack = vmm::alloc_contiguous_virtual_pages(SCHED_SYSTEM_STACK_PAGES, DEFAULT_MAPPING_FLAGS);
    if (!system_stack) {
        delete task;
        vmm::unmap_contiguous_virtual_pages(reinterpret_cast<uintptr_t>(task_stack), SCHED_TASK_STACK_PAGES);
        return nullptr;
    }

    task->task_stack = reinterpret_cast<uint64_t>(task_stack);
    task->system_stack = reinterpret_cast<uint64_t>(system_stack);

    // Initialize the CPU context
    task->cpu_context.hwframe.rip = reinterpret_cast<uint64_t>(entry);        // Set instruction pointer to the task function
    task->cpu_context.hwframe.rflags = 0x200;                                 // Enable interrupts
    task->cpu_context.hwframe.rsp = task->task_stack + SCHED_TASK_STACK_SIZE; // Point to the top of the stack
    task->cpu_context.rbp = task->cpu_context.hwframe.rsp;                    // Point to the top of the stack
    task->cpu_context.rdi = reinterpret_cast<uint64_t>(task_data);            // Task parameter buffer pointer

    // Set up segment registers for kernel space. These values correspond to the selectors in the GDT.
    uint64_t data_segment = __KERNEL_DS;
    task->cpu_context.ds = data_segment;
    task->cpu_context.es = data_segment;
    task->cpu_context.hwframe.ss = data_segment;
    task->cpu_context.hwframe.cs = __KERNEL_CS;

    return task;
}
} // namespace sched
