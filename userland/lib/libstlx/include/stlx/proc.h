#ifndef STLX_PROC_H
#define STLX_PROC_H

#include <stdint.h>

/* Wait status decode macros (Linux-compatible bit layout). */
#define STLX_WIFEXITED(s)    (((s) & 0x7F) == 0)
#define STLX_WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define STLX_WIFSIGNALED(s)  (((s) & 0x7F) != 0)
#define STLX_WTERMSIG(s)     ((s) & 0x7F)

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

/**
 * Kill a child process. The child is terminated asynchronously. The
 * handle remains valid; call proc_wait() afterward to collect the
 * exit status. Returns 0 on success, -1 on failure.
 */
int proc_kill(int handle);

/**
 * Create a thread in the caller's address space. The thread shares the
 * caller's mm_context and gets a copy of the caller's handle table and cwd.
 * Returns a handle in CREATED state; call proc_thread_start() to schedule.
 * The entry function MUST call _exit() -- returning from it is undefined.
 *
 * @param entry  User-space function pointer (thread entry point).
 * @param arg    Argument passed via first register.
 * @param stack_top  Top of the caller-allocated stack (stacks grow down).
 * @param name   Debug name for the thread.
 * @return Handle on success, negative errno on failure.
 */
int proc_create_thread(void (*entry)(void*), void* arg,
                       void* stack_top, const char* name);

static inline int proc_thread_start(int handle) { return proc_start(handle); }
static inline int proc_thread_join(int handle, int* exit_code) { return proc_wait(handle, exit_code); }
static inline int proc_thread_detach(int handle) { return proc_detach(handle); }
static inline int proc_thread_kill(int handle) { return proc_kill(handle); }

#endif /* STLX_PROC_H */
