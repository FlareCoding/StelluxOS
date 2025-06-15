#include <process/process_env.h>
#include <core/sync.h>

// Define the static idle process environment
process_env g_idle_process_env;

// Lock to ensure no identical PIDs get produced
DECLARE_GLOBAL_OBJECT(mutex, g_eid_alloc_lock);
eid_t g_available_env_eid = 1;

eid_t alloc_environment_id() {
    mutex_guard guard(g_eid_alloc_lock);
    return g_available_env_eid++;
}
