#ifndef STLIBC_PROC_H
#define STLIBC_PROC_H

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

// Process creation flags
enum proc_flags_t {
    PROC_NONE           = 0 << 0,  // Invalid / empty flags
    PROC_SHARE_ENV      = 1 << 0,  // Share environment with parent
    PROC_COPY_ENV       = 1 << 1,  // Copy parent's environment
    PROC_NEW_ENV        = 1 << 2,  // Create new environment
    PROC_CAN_ELEVATE    = 1 << 3,  // Create new environment
};

// Process access rights
enum proc_access_t {
    PROC_ACCESS_NONE    = 0 << 0,  // No access
    PROC_ACCESS_READ    = 1 << 0,  // Read access
    PROC_ACCESS_WRITE   = 1 << 1,  // Write access
    PROC_ACCESS_EXECUTE = 1 << 2,  // Execute access
    PROC_ACCESS_ALL     = PROC_ACCESS_READ | PROC_ACCESS_WRITE | PROC_ACCESS_EXECUTE
};

// Process handle flags
enum proc_handle_flags_t {
    PROC_HANDLE_NONE    = 0 << 0,  // No special flags
    PROC_HANDLE_INHERIT = 1 << 0,  // Handle is inherited by child processes
    PROC_HANDLE_PROTECT = 1 << 1   // Handle cannot be closed
};

/**
 * @struct proc_info
 * @brief Information about a created process.
 */
struct proc_info {
    pid_t pid;          // Process ID
    char name[256];     // Process name
};

/**
 * @brief Creates a new process by loading an executable file.
 * @param path Path to the executable file
 * @param flags Process creation flags
 * @param access_rights Access rights for the process handle
 * @param handle_flags Handle flags
 * @param info Pointer to store process information (can be NULL)
 * @return int32_t The process handle, or -1 on error
 */
int proc_create(const char* path, uint64_t flags, uint32_t access_rights, uint32_t handle_flags, struct proc_info* info);

/**
 * @brief Waits for a process to terminate.
 * @param handle Process handle to wait for
 * @param exit_code Pointer to store the exit code (can be NULL)
 * @return 0 on success, -1 on error
 */
int proc_wait(int handle, int* exit_code);

/**
 * @brief Closes a process handle.
 * @param handle Process handle to close
 * @return 0 on success, -1 on error
 */
int proc_close(int handle);

#ifdef __cplusplus
}
#endif

#endif // STLIBC_PROC_H
