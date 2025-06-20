#ifndef PROCESS_CORE_H
#define PROCESS_CORE_H

#include "ptregs.h"
#include "fpu.h"
#include "mm.h"
#include <arch/percpu.h>

#define MAX_PROCESS_NAME_LEN 255

typedef int64_t pid_t;

/**
 * @brief Allocates a new process ID.
 * 
 * This function atomically allocates a new process ID by incrementing
 * a global counter. The allocation is protected by a mutex to ensure
 * no duplicate PIDs are generated.
 * 
 * @return pid_t The newly allocated process ID.
 */
pid_t alloc_process_id();

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
 * @struct process_core
 * @brief Represents the essential execution state of a process.
 * 
 * This structure contains all the hardware and execution state necessary
 * to run a process, independent of its environment. It can be snapshotted
 * and restored in different environments.
 */
struct process_core {
    /**
     * @brief CPU register state of the process.
     * 
     * Contains all general purpose registers, segment registers,
     * and other CPU state needed for execution.
     */
    ptregs cpu_context;

    /**
     * @brief FPU/SSE register state of the process.
     * 
     * Contains the complete floating point state including
     * XMM registers, control/status registers, and tracking flags.
     */
    fpu::fpu_state fpu_context __attribute__((aligned(__FPU_ALIGNMENT)));

    /**
     * @brief Hardware-specific state flags.
     * 
     * Contains various hardware state flags and CPU information
     * needed for proper execution and scheduling.
     */
    struct {
        uint64_t elevated : 1;    // Hardware privilege state
        uint64_t cpu      : 8;    // Current CPU core
        uint64_t flrsvd   : 55;   // Reserved for future use
    } __attribute__((packed)) hw_state;
    
    /**
     * @brief Memory management context.
     * 
     * Contains page tables, memory mappings, and other
     * memory management related state.
     */
    mm_context mm_ctx;
    
    /**
     * @brief Stack information for the process.
     * 
     * Contains both the task stack (used for normal execution)
     * and system stack (used for interrupts and system calls).
     */
    struct {
        uint64_t task_stack;        // Base address of the task stack
        uint64_t task_stack_top;    // Top address of the task stack
        uint64_t system_stack;      // Base address of the system stack
        uint64_t system_stack_top;  // Top address of the system stack
    } stacks;
    
    /**
     * @brief Current execution state of the process.
     * 
     * Tracks whether the process is running, waiting,
     * terminated, etc.
     */
    process_state state;

    /**
     * @brief Process identity information.
     */
    struct {
        pid_t pid;                              // Process ID
        char name[MAX_PROCESS_NAME_LEN + 1];    // Process name
    } identity;

    /**
     * @brief Exit code of the process.
     * 
     * Stores the exit code that will be returned when the process
     * terminates. This value is set by the process itself or by
     * the kernel when the process is terminated.
     */
    int exit_code;

    /**
     * @brief Context switch state flags.
     * 
     * Contains flags that indicate the state of the process
     * during context switching. These flags are used by the
     * interrupt handler to perform necessary cleanup after
     * a context switch is complete.
     */
    struct {
        uint64_t needs_cleanup  : 1;    // Indicate to the process that called `release_ref()` that the process
                                        // needs to be added to the scheduler's cleanup queue, so the calling
                                        // process can do it potentially without `RUN_ELEVATED()` call.
        uint64_t reserved       : 63;   // Reserved for future use
    } __attribute__((packed)) ctx_switch_state;

    /**
     * @brief Thread Local Storage (TLS) base address.
     * 
     * Stores the base address for the thread's local storage.
     * This is used by the kernel to manage thread-specific data
     * and is typically accessed through the FS segment register.
     */
    uint64_t fs_base;
};

#endif // PROCESS_CORE_H
 