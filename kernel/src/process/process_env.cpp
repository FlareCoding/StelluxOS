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

size_t process_env::handle_table::add_handle(uint64_t id, void* object) {
    for (size_t i = 0; i < MAX_HANDLES; i++) {
        if (entries[i].id == 0) {
            entries[i].id = id;
            entries[i].__object = object;
            return i;
        }
    }
    return SIZE_MAX;
}

bool process_env::handle_table::remove_handle(size_t index) {
    if (index >= MAX_HANDLES || entries[index].id == 0) {
        return false;
    }

    entries[index].id = 0;
    entries[index].__object = nullptr;
    return true;
}

size_t process_env::handle_table::find_handle(uint64_t id) {
    for (size_t i = 0; i < MAX_HANDLES; i++) {
        if (entries[i].id != 0 && entries[i].id == id) {
            return i;
        }
    }
    return SIZE_MAX;
}
