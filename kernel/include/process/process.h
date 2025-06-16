#ifndef PROCESS_H
#define PROCESS_H
#include "process_core.h"
#include "process_env.h"
#include <memory/paging.h>
#include <memory/memory.h>
#include <core/sync.h>

// #define ENABLE_PROC_LIFECYCLE_TRACES

// Forward declaration of process class
class process;

/**
 * @brief Defines the type for task entry functions.
 * 
 * A function of this type is called when a new kernel thread starts executing.
 */
typedef void (*task_entry_fn_t)(void*);

/**
 * @brief Per-CPU variable for the current process.
 */
DECLARE_PER_CPU(process*, current_process);

/**
 * @brief Per-CPU variable for the current process core.
 */
DECLARE_PER_CPU(process_core*, current_process_core);

/**
 * @brief Per-CPU variable for the top of the current system stack.
 */
DECLARE_PER_CPU(uint64_t, current_system_stack);

/**
 * @brief Retrieves the current process for the executing CPU.
 * @return Pointer to the current process.
 */
static __force_inline__ process* get_current_process() {
    return this_cpu_read(current_process);
}

/**
 * @brief Retrieves the current process core for the executing CPU.
 * @return Pointer to the current process core.
 */
static __force_inline__ process_core* get_current_process_core() {
    return this_cpu_read(current_process_core);
}

/**
 * @brief Macro for accessing the current process.
 */
#define current get_current_process()

/**
 * @brief Macro for accessing the current process core.
 */
#define current_task get_current_process_core()

namespace sched {
/**
 * @brief Saves CPU context into the process control block.
 * @param process_context Pointer to the process's saved context.
 * @param irq_frame Pointer to the interrupt frame.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void save_cpu_context(ptregs* process_context, ptregs* irq_frame);

/**
 * @brief Restores CPU context from the process control block.
 * @param process_context Pointer to the process's saved context.
 * @param irq_frame Pointer to the interrupt frame.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void restore_cpu_context(ptregs* process_context, ptregs* irq_frame);

/**
 * @brief Switches context during an IRQ interrupt.
 * @param old_cpu ID of the old CPU.
 * @param new_cpu ID of the new CPU.
 * @param from Pointer to the process being switched out.
 * @param to Pointer to the process being switched in.
 * @param irq_frame Pointer to the interrupt frame.
 * 
 * This function is called from an IRQ handler to perform context switching.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void switch_context_in_irq(
    int old_cpu,
    int new_cpu,
    process* from,
    process* to,
    ptregs* irq_frame
);

/**
 * @brief Creates a privileged kernel process core.
 * @param entry Entry function for the process.
 * @param process_data Pointer to data passed to the process.
 * @return Pointer to the created process core.
 * 
 * The process core starts in privileged mode (DPL=0).
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE process_core* create_priv_kernel_process_core(task_entry_fn_t entry, void* process_data);

/**
 * @brief Creates an unprivileged kernel process core.
 * @param entry Entry function for the process.
 * @param process_data Pointer to data passed to the process.
 * @return Pointer to the created process core.
 * 
 * The process core starts in unprivileged mode (DPL=3).
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE process_core* create_unpriv_kernel_process_core(task_entry_fn_t entry, void* process_data);

/**
 * @brief Creates a userland process core.
 * @param entry_addr Entry point address for the process.
 * @param pt Page table for the process.
 * @return Pointer to the created process core.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE process_core* create_userland_process_core(
    uintptr_t entry_addr,
    paging::page_table* pt
);

/**
 * @brief Destroys a process core, releasing its resources.
 * @param core Pointer to the process core to destroy.
 * @return True if the process core was successfully destroyed, false otherwise.
 * 
 * Frees all memory and resources associated with the process core.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool destroy_process_core(process_core* core);

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
__PRIVILEGED_CODE bool map_userland_process_stack(
    paging::page_table* pt,
    uint64_t& out_stack_bottom,
    uint64_t& out_stack_top
);

/**
 * @brief Terminates the current kernel thread and switches to the next process.
 * 
 * If no valid process is available, the kernel swapper/idle task is executed.
 */
void exit_process();

/**
 * @brief Relinquishes the CPU and forces a context switch.
 * 
 * Causes the current process to yield the CPU to the next available task.
 */
void yield();
} // namespace sched

/**
 * @class process
 * @brief Represents a process in the system, combining core execution state and environment.
 * 
 * This class manages both the process core (execution state) and environment (context).
 * It provides methods for process lifecycle management, resource control, and state
 * manipulation.
 */
class process {
public:
    /**
     * @brief Default constructor.
     * 
     * Creates an uninitialized process. One of the init methods must be called before use.
     * Initializes reference count to 1 for self-ownership.
     */
    process() : m_ref_count(1) {}

    /**
     * @brief Default destructor.
     * 
     * Note: cleanup() should be called before destruction to properly free resources.
     */
    ~process() = default;

    /**
     * @brief Increments the process reference count.
     * 
     * This should be called when creating a new handle to this process.
     * The reference count is protected by a mutex to ensure thread safety.
     */
    void add_ref();

    /**
     * @brief Decrements the process reference count.
     * 
     * This should be called when closing a handle to this process.
     * If the reference count reaches 0, the process is cleaned up and deleted.
     * The reference count is protected by a mutex to ensure thread safety.
     * 
     * @return true if the process was deleted, false otherwise
     */
    bool release_ref();

    /**
     * @brief Gets the current reference count.
     * 
     * @return The current reference count
     */
    uint64_t get_ref_count() const;

    /**
     * @brief Initializes a process with an entry point and data.
     * @param name Name for the process.
     * @param entry Entry function for the process.
     * @param data Data to pass to the entry function.
     * @param flags Flags controlling process creation behavior.
     * @return true if initialization was successful, false otherwise.
     * 
     * Creates both a new process core and environment. The process takes ownership of both.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool init_with_entry(const char* name, task_entry_fn_t entry, void* data, process_creation_flags flags);

    /**
     * @brief Initializes a process with an entry point, data, and existing environment.
     * @param name Name for the process.
     * @param entry Entry function for the process.
     * @param data Data to pass to the entry function.
     * @param env The process environment to use.
     * @param flags Flags controlling process creation behavior.
     * @param take_ownership Whether to take ownership of the environment.
     * @return true if initialization was successful, false otherwise.
     * 
     * Creates a new process core and uses the provided environment. The process takes ownership
     * of the core and optionally takes ownership of the environment.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool init_with_entry(const char* name, task_entry_fn_t entry, void* data, process_env* env, process_creation_flags flags, bool take_ownership = false);

    /**
     * @brief Initializes a process with an existing core.
     * @param core The core execution state to use.
     * @param flags Flags controlling process creation behavior.
     * @param take_ownership Whether to take ownership of the core.
     * @return true if initialization was successful, false otherwise.
     * 
     * Uses the provided core and creates a new environment. The process optionally takes ownership
     * of the core and takes ownership of the new environment.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool init_with_core(process_core* core, process_creation_flags flags, bool take_ownership = false);

    /**
     * @brief Initializes a process with an existing environment.
     * @param name Name for the process.
     * @param env The process environment to use.
     * @param flags Flags controlling process creation behavior.
     * @param take_ownership Whether to take ownership of the environment.
     * @return true if initialization was successful, false otherwise.
     * 
     * Creates a new process core and uses the provided environment. The process takes ownership
     * of the new core and optionally takes ownership of the environment.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool init_with_env(const char* name, process_env* env, process_creation_flags flags, bool take_ownership = false);

    /**
     * @brief Initializes a process with just creation flags.
     * @param name Optional name for the process. If nullptr, no name is set.
     * @param flags Flags controlling process creation behavior.
     * @return true if initialization was successful, false otherwise.
     * 
     * Creates both a new process core and environment. The process takes ownership of both.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool init_with_flags(const char* name, process_creation_flags flags);

    /**
     * @brief Initializes a process with existing core and environment.
     * @param core The core execution state to use.
     * @param take_core_ownership Whether to take ownership of the core.
     * @param env The process environment to use.
     * @param take_env_ownership Whether to take ownership of the environment.
     * @return true if initialization was successful, false otherwise.
     * 
     * Uses the provided core and environment. The process optionally takes ownership of both.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool init(process_core* core, bool take_core_ownership, process_env* env, bool take_env_ownership);

    /**
     * @brief Cleans up process resources.
     * 
     * This should be called before the process is destroyed to ensure
     * proper cleanup of resources.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void cleanup();

    /**
     * @brief Gets the current process core.
     * @return Pointer to the process core, or nullptr if not initialized.
     */
    process_core* get_core() const { return m_core; }

    /**
     * @brief Gets the current process environment.
     * @return Pointer to the process environment, or nullptr if not initialized.
     */
    process_env* get_env() const { return m_env; }

    /**
     * @brief Checks if the process is properly initialized.
     * @return true if the process is initialized, false otherwise.
     */
    bool is_initialized() const { return m_is_initialized; }

    /**
     * @brief Starts the process execution.
     * @return true if the process was successfully started, false otherwise.
     */
    bool start();

    /**
     * @brief Pauses the process execution.
     * @return true if the process was successfully paused, false otherwise.
     */
    bool pause();

    /**
     * @brief Resumes the process execution.
     * @return true if the process was successfully resumed, false otherwise.
     */
    bool resume();

    /**
     * @brief Terminates the process.
     * @return true if the process was successfully terminated, false otherwise.
     */
    bool terminate();

private:
    process_core* m_core = nullptr;       // Core execution state
    process_env* m_env = nullptr;         // Process environment
    bool m_is_initialized = false;        // Whether the process has been properly initialized
    bool m_owns_core = false;             // Whether this process owns and should delete the core
    bool m_owns_env = false;              // Whether this process owns and should delete the environment
    atomic<uint64_t> m_ref_count{1};      // Reference count for the process

    /**
     * @brief Creates a process core based on flags and entry point.
     * @param entry Entry function for the process, or nullptr if none.
     * @param data Data to pass to the entry function.
     * @param flags Flags controlling process creation behavior.
     * @return Pointer to the created process core, or nullptr if creation failed.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE process_core* _create_process_core(task_entry_fn_t entry, void* data, process_creation_flags flags);
};

#endif // PROCESS_H
