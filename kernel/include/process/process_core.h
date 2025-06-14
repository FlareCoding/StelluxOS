#ifndef PROCESS_CORE_H
#define PROCESS_CORE_H

#include "ptregs.h"
#include "mm.h"
#include <arch/percpu.h>

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
     * @brief Current execution state of the process.
     * 
     * Tracks whether the process is running, waiting,
     * terminated, etc.
     */
    process_state state;
};

#endif // PROCESS_CORE_H
 