#include "resource/handle_table.h"
#include "resource/resource.h"

namespace resource {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_handle_table(handle_table* table) {
    if (!table) {
        return;
    }

    table->lock = sync::SPINLOCK_INIT;
    for (uint32_t i = 0; i < MAX_TASK_HANDLES; i++) {
        table->entries[i].used = false;
        table->entries[i].generation = 0;
        table->entries[i].reserved = 0;
        table->entries[i].rights = 0;
        table->entries[i].type = resource_type::UNKNOWN;
        table->entries[i].obj = nullptr;
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t alloc_handle(
    handle_table* table,
    resource_object* obj,
    resource_type type,
    uint32_t rights,
    handle_t* out_handle
) {
    if (!table || !obj || !out_handle) {
        return HANDLE_ERR_INVAL;
    }
    if (type == resource_type::UNKNOWN) {
        return HANDLE_ERR_INVAL;
    }
    if ((rights & ~RIGHT_MASK) != 0) {
        return HANDLE_ERR_INVAL;
    }

    sync::irq_lock_guard guard(table->lock);
    for (uint32_t i = 0; i < MAX_TASK_HANDLES; i++) {
        handle_entry& entry = table->entries[i];
        if (entry.used) {
            continue;
        }

        resource_add_ref(obj);
        entry.used = true;
        entry.generation++;
        entry.rights = rights;
        entry.type = type;
        entry.obj = obj;

        *out_handle = static_cast<handle_t>(i);
        return HANDLE_OK;
    }

    return HANDLE_ERR_NOSPC;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t get_handle_object(
    handle_table* table,
    handle_t handle,
    uint32_t required_rights,
    resource_object** out_obj
) {
    if (!table || !out_obj) {
        return HANDLE_ERR_INVAL;
    }
    if (handle < 0 || static_cast<uint32_t>(handle) >= MAX_TASK_HANDLES) {
        return HANDLE_ERR_NOENT;
    }
    if ((required_rights & ~RIGHT_MASK) != 0) {
        return HANDLE_ERR_INVAL;
    }

    sync::irq_lock_guard guard(table->lock);
    handle_entry& entry = table->entries[static_cast<uint32_t>(handle)];
    if (!entry.used || !entry.obj || entry.type == resource_type::UNKNOWN) {
        return HANDLE_ERR_NOENT;
    }
    if ((entry.rights & required_rights) != required_rights) {
        return HANDLE_ERR_ACCESS;
    }

    resource_add_ref(entry.obj);
    *out_obj = entry.obj;
    return HANDLE_OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t remove_handle(
    handle_table* table,
    handle_t handle,
    resource_object** out_obj
) {
    if (!table || !out_obj) {
        return HANDLE_ERR_INVAL;
    }
    if (handle < 0 || static_cast<uint32_t>(handle) >= MAX_TASK_HANDLES) {
        return HANDLE_ERR_NOENT;
    }

    sync::irq_lock_guard guard(table->lock);
    handle_entry& entry = table->entries[static_cast<uint32_t>(handle)];
    if (!entry.used || !entry.obj || entry.type == resource_type::UNKNOWN) {
        return HANDLE_ERR_NOENT;
    }

    resource_object* obj = entry.obj;
    entry.used = false;
    entry.rights = 0;
    entry.type = resource_type::UNKNOWN;
    entry.obj = nullptr;
    entry.reserved = 0;

    *out_obj = obj;
    return HANDLE_OK;
}

} // namespace resource
