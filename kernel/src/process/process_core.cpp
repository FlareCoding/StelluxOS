#include <process/process_core.h>
#include <core/sync.h>

// Lock to ensure no identical PIDs get produced
DECLARE_GLOBAL_OBJECT(mutex, g_pid_alloc_lock);
pid_t g_available_process_pid = 1;

pid_t alloc_process_id() {
    mutex_guard guard(g_pid_alloc_lock);
    return g_available_process_pid++;
}

