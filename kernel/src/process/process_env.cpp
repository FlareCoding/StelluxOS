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

size_t process_env::handle_table::add_handle(handle_type type, void* object, uint32_t access_rights, uint32_t flags, uint64_t metadata) {
    for (size_t i = 0; i < MAX_HANDLES; i++) {
        if (entries[i].type == handle_type::INVALID) {
            entries[i].type = type;
            entries[i].object = object;
            entries[i].access_rights = access_rights;
            entries[i].flags = flags;
            entries[i].metadata = metadata;
            return i;
        }
    }
    return SIZE_MAX;
}

bool process_env::handle_table::remove_handle(int32_t handle) {
    if (handle < 0 || static_cast<size_t>(handle) >= MAX_HANDLES || entries[handle].type == handle_type::INVALID) {
        return false;
    }

    entries[handle].type = handle_type::INVALID;
    entries[handle].object = nullptr;
    entries[handle].access_rights = 0;
    entries[handle].flags = 0;
    entries[handle].metadata = 0;
    return true;
}

handle_entry* process_env::handle_table::get_handle(int32_t handle) {
    if (handle < 0 || static_cast<size_t>(handle) >= MAX_HANDLES || entries[handle].type == handle_type::INVALID) {
        return nullptr;
    }
    return &entries[handle];
}

size_t process_env::handle_table::find_handle_by_object(void* object) {
    if (!object) {
        return SIZE_MAX;
    }

    for (size_t i = 0; i < MAX_HANDLES; i++) {
        if (entries[i].type != handle_type::INVALID && entries[i].object == object) {
            return i;
        }
    }
    return SIZE_MAX;
}
