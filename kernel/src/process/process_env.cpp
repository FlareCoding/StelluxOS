#include <process/process_env.h>
#include <core/sync.h>

// Define the static idle process environment
process_env g_idle_process_env;

// Lock to ensure no identical PIDs get produced
DECLARE_GLOBAL_OBJECT(mutex, g_pid_alloc_lock);
pid_t g_available_process_pid = 1;

pid_t alloc_process_pid() {
    mutex_guard guard(g_pid_alloc_lock);
    return g_available_process_pid++;
}
