#ifndef STLIBC_PROC_H
#define STLIBC_PROC_H

#include <stlibc/proc/pid.h>

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

/**
 * @brief Creates a new process by loading an executable file.
 * @param path Path to the executable file
 * @param flags Process creation flags
 * @return pid_t The process ID of the created process, or -1 on error
 */
pid_t proc_create(const char* path, uint64_t flags);

/**
 * @brief Waits for a process to terminate.
 * @param pid Process ID to wait for
 * @param exit_code Pointer to store the exit code (can be NULL)
 * @return 0 on success, -1 on error
 */
int proc_wait(pid_t pid, int* exit_code);

#ifdef __cplusplus
}
#endif

#endif // STLIBC_PROC_H
