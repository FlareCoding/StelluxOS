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
} // namespace sched

#endif // PROCESS_H
