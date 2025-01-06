#ifndef PROCESS_H
#define PROCESS_H
#include "ptregs.h"
#include "mm.h"
#include <memory/paging.h>
#include <arch/percpu.h>

#define MAX_PROCESS_NAME_LEN 255

typedef int64_t pid_t;

/**
 * @enum process_state
 * @brief Represents the state of a process.
 */
enum class process_state {
    INVALID = 0, // Process doesn't exist
    NEW,        // Process created but not yet ready to run
    READY,      // Ready to be scheduled
    RUNNING,    // Currently executing
    WAITING,    // Waiting for some resource
    TERMINATED  // Finished execution
};

/**
 * @struct task_control_block
 * @brief Represents the control block for a task or process.
 * 
 * Contains all information needed to manage and schedule a task, including
 * CPU context, state, PID, stacks, and task name.
 */
struct task_control_block {
    ptregs          cpu_context;
    process_state   state;
    pid_t           pid;

    // Primary execution stack used by a thread
    uint64_t        task_stack;
    uint64_t        task_stack_top;

    // Secondary stack used for system work
    // in the syscall and interrupt contexts.
    uint64_t        system_stack;
    uint64_t        system_stack_top;

    struct {
        // Indicates if the task is currently in a hw-privileged state.
        uint64_t    elevated    : 1;

        // CPU core that the task is currently running/schedulable on
        uint64_t    cpu         : 8;

        // Reserved flags
        uint64_t    flrsvd      : 55;
    } __attribute__((packed));

    // MMU-specific context
    mm_context      mm_ctx;

    char            name[MAX_PROCESS_NAME_LEN + 1];
};

/**
 * @brief Per-CPU variable for the current task.
 */
DECLARE_PER_CPU(task_control_block*, current_task);

/**
 * @brief Per-CPU variable for the top of the current system stack.
 */
DECLARE_PER_CPU(uint64_t, current_system_stack);

/**
 * @brief Retrieves the current task for the executing CPU.
 * @return Pointer to the `task_control_block` of the current task.
 */
static __force_inline__ task_control_block* get_current_task() {
    return this_cpu_read(current_task);
}

/**
 * @brief Macro for accessing the current task's control block.
 */
#define current get_current_task()

namespace sched {
/**
 * @typedef task_entry_fn_t
 * @brief Defines the type for task entry functions.
 * 
 * A function of this type is called when a new kernel thread starts executing.
 */
typedef void (*task_entry_fn_t)(void*);

/**
 * @brief Saves CPU context into the process control block.
 * @param process_context Pointer to the task's saved context.
 * @param irq_frame Pointer to the interrupt frame.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void save_cpu_context(ptregs* process_context, ptregs* irq_frame);

/**
 * @brief Restores CPU context from the process control block.
 * @param process_context Pointer to the task's saved context.
 * @param irq_frame Pointer to the interrupt frame.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void restore_cpu_context(ptregs* process_context, ptregs* irq_frame);

/**
 * @brief Switches context during an IRQ interrupt.
 * @param old_cpu ID of the old CPU.
 * @param new_cpu ID of the new CPU.
 * @param from Pointer to the task being switched out.
 * @param to Pointer to the task being switched in.
 * @param irq_frame Pointer to the interrupt frame.
 * 
 * This function is called from an IRQ handler to perform context switching.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void switch_context_in_irq(
    int old_cpu,
    int new_cpu,
    task_control_block* from,
    task_control_block* to,
    ptregs* irq_frame
);

/**
 * @brief Creates a privileged kernel task.
 * @param entry Entry function for the task.
 * @param task_data Pointer to data passed to the task.
 * @return Pointer to the `task_control_block` for the created task.
 * 
 * The task starts in privileged mode (DPL=0).
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task_control_block* create_priv_kernel_task(task_entry_fn_t entry, void* task_data);

/**
 * @brief Creates an unprivileged kernel task.
 * @param entry Entry function for the task.
 * @param task_data Pointer to data passed to the task.
 * @return Pointer to the `task_control_block` for the created task.
 * 
 * The task starts in unprivileged mode (DPL=3).
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task_control_block* create_unpriv_kernel_task(task_entry_fn_t entry, void* task_data);

/**
 * @brief TBD.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task_control_block* create_upper_class_userland_task(
    uintptr_t entry_addr,
    paging::page_table* pt
);

/**
 * @brief Destroys a task, releasing its resources.
 * @param task Pointer to the `task_control_block` of the task to destroy.
 * @return True if the task was successfully destroyed, false otherwise.
 * 
 * Frees all memory and resources associated with the task.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool destroy_task(task_control_block* task);

/**
 * @brief Creates a new system stack.
 * 
 * This function allocates a new system stack for use in interrupt contexts.
 * The base address of the allocated stack is returned, and the top address
 * (used for initializing the stack pointer) is written to `out_stack_top`.
 * 
 * @param[out] out_stack_top A reference to a variable where the top address of
 *                           the newly created stack will be stored. This address
 *                           should be used as the stack pointer when starting the task.
 * 
 * @return uint64_t The base address of the newly allocated stack. Returns 0 if
 *                  stack allocation fails.
 * 
 * The created stack is aligned to `PAGE_SIZE` and has sufficient space for typical
 * kernel tasks.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint64_t allocate_system_stack(uint64_t& out_stack_top);

/**
 * @brief Maps a userland task stack into virtual memory.
 * 
 * This function allocates and maps a stack for a userland task. The stack is aligned to
 * `PAGE_SIZE` and spans a predefined range in the userland address space. The bottom and
 * top addresses of the stack are written to `out_stack_bottom` and `out_stack_top`, respectively.
 *
 * @param pt Page table to map the userland task stack into.
 * 
 * @param[out] out_stack_bottom A reference to a variable where the bottom address of the
 *                              stack will be stored. This address represents the lowest 
 *                              accessible address of the stack.

 * @param[out] out_stack_top A reference to a variable where the top address of the
 *                           stack will be stored. This address represents the initial
 *                           stack pointer value.
 * 
 * @return bool `true` if the stack was successfully mapped, `false` otherwise.
 * 
 * The stack is mapped in a user-accessible region, ensuring proper permissions for userland tasks.
 * The stack top is typically mapped to a high address, such as `0x00007fffffffffff`, while the 
 * bottom address depends on the allocated stack size.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool map_userland_task_stack(
    paging::page_table* pt,
    uint64_t& out_stack_bottom,
    uint64_t& out_stack_top
);

/**
 * @brief Terminates the current kernel thread and switches to the next task.
 * 
 * If no valid task is available, the kernel swapper/idle task is executed.
 */
void exit_thread();

/**
 * @brief Relinquishes the CPU and forces a context switch.
 * 
 * Causes the current task to yield the CPU to the next available task.
 */
void yield();
} // namespace sched

#endif // PROCESS_H
