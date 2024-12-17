#ifndef PROCESS_H
#define PROCESS_H
#include "ptregs.h"
#include <arch/percpu.h>

#define MAX_PROCESS_NAME_LEN 255

typedef int64_t pid_t;

enum class process_state {
    INVALID = 0, // Process doesn't exist
    NEW,        // Process created but not yet ready to run
    READY,      // Ready to be scheduled
    RUNNING,    // Currently executing
    WAITING,    // Waiting for some resource
    TERMINATED  // Finished execution
};

struct task_control_block {
    ptregs          cpu_context;
    process_state   state;
    pid_t           pid;

    // Primary execution stack used by a thread
    uint64_t        task_stack;

    // Secondary stack used for system work
    // in the syscall and interrupt contexts.
    uint64_t        system_stack;

    struct {
        uint64_t    elevated    : 1;
        uint64_t    cpu         : 8;
        uint64_t    flrsvd      : 55;
    } __attribute__((packed));

    char            name[MAX_PROCESS_NAME_LEN + 1];
};

DECLARE_PER_CPU(task_control_block*, current_task);

static __force_inline__ task_control_block* get_current_task() {
    return this_cpu_read(current_task);
}

#define current get_current_task()

namespace sched {
typedef void (*task_entry_fn_t)(void*);

// Saves context registers from the interrupt frame into a CPU context struct
__PRIVILEGED_CODE void save_cpu_context(ptregs* process_context, ptregs* irq_frame);

// Saves context registers from the CPU context struct into an interrupt frame
__PRIVILEGED_CODE void restore_cpu_context(ptregs* process_context, ptregs* irq_frame);

// Saves and restores necessary registers into the appropriate
// process control blocks using an interrupt frame.
// *Note* Meant to be called from within an interrupt handler
// and context would get switched upon interrupt return.
__PRIVILEGED_CODE void switch_context_in_irq(
    int old_cpu,
    int new_cpu,
    task_control_block* from,
    task_control_block* to,
    ptregs* irq_frame
);

//
// Allocates a task object for a new kernel thread that will
// start its execution at a given function in kernel mode (DPL=0).
//
__PRIVILEGED_CODE task_control_block* create_kernel_task(task_entry_fn_t entry, void* task_data);

//
// Destroys a task object, releasing any resources allocated for the task.
// This function should properly clean up any state or memory associated 
// with the task, ensuring it no longer runs and freeing up any used memory.
//
// Parameters:
// - task: A pointer to the Task object to be destroyed.
//         The Task pointer must not be used after calling this function.
//
// Returns:
// - Returns true if the task was successfully destroyed. False if there
//   was an error (such as the task not being found).
//
__PRIVILEGED_CODE bool destroy_task(task_control_block* task);

//
// Allows the current running kernel thread to terminate and switch to the next
// available task without waiting for the next timer interrupt. If no next valid
// task is available, control flow switches back to the kernel swapper task.
//
__PRIVILEGED_CODE void exit_thread();

//
// Relinquishes the CPU and causes a context switch to switch to the next
// available task without waiting for the next timer tick. If no next valid
// task is available, control flow switches back to the current task.
//
__PRIVILEGED_CODE void yield();
} // namespace sched

#endif // PROCESS_H
