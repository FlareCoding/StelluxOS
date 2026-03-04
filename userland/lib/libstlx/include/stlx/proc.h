#ifndef STLX_PROC_H
#define STLX_PROC_H

#include <stdint.h>

typedef struct {
    char name[256];
    int pid;
    int cpu;
} process_info;

/**
 * Create a process from an ELF binary. Loads the ELF, creates a task in
 * TASK_STATE_CREATED (not scheduled). Returns a handle on success, -1 on
 * failure with errno set.
 */
int proc_create(const char* path, const char* argv[]);

/**
 * Convenience: proc_create + proc_start in one call. Returns handle on
 * success, -1 on failure with errno set.
 */
int proc_exec(const char* path, const char* argv[]);

/**
 * Schedule a created (but not yet running) task. Returns 0 on success,
 * -1 on failure with errno set.
 */
int proc_start(int handle);

/**
 * Block until the child process exits. Stores the exit code in *exit_code.
 * Consumes the handle (handle becomes invalid after this call).
 * Returns 0 on success, -1 on failure with errno set.
 */
int proc_wait(int handle, int* exit_code);

/**
 * Detach ownership of a child process. The handle becomes invalid. The
 * child continues running independently; when it exits, the system reaper
 * cleans it up. Returns 0 on success, -1 on failure with errno set.
 */
int proc_detach(int handle);

/**
 * Query process information. Handle must still be valid (call before
 * proc_wait/proc_detach). Returns 0 on success, -1 on failure with
 * errno set.
 */
int proc_info(int handle, process_info* info);

/**
 * Install a resource handle at a specific fd slot in a child process.
 * The child must be in CREATED state (not yet started). Replaces any
 * existing handle at that slot. Returns 0 on success, -1 on failure.
 */
int proc_set_handle(int proc_handle, int slot, int resource_handle);

#endif /* STLX_PROC_H */
