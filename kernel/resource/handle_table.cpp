#include "resource/handle_table.h"
#include "resource/resource.h"
#include "mm/heap.h"

namespace resource {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void handle_table::ref_destroy(handle_table* self) {
    if (!self) {
        return;
    }

    for (uint32_t i = 0; i < self->capacity; i++) {
        resource_object* obj = nullptr;

        if (remove_handle(self, static_cast<handle_t>(i), &obj) == HANDLE_OK) {
            resource_release(obj);
        }
    }

    heap::kfree(self->entries);
    heap::kfree_delete(self);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_handle_table(handle_table* table) {
    if (!table) {
        return HANDLE_ERR_INVAL;
    }

    // An all-zero entry is an empty slot
    auto* entries = static_cast<handle_entry*>(heap::kzalloc(MAX_TASK_HANDLES * sizeof(handle_entry)));
    if (!entries) {
        return HANDLE_ERR_NOMEM;
    }

    table->lock = sync::SPINLOCK_INIT;
    table->entries = entries;
    table->capacity = MAX_TASK_HANDLES;

    return HANDLE_OK;
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

    for (uint32_t i = 0; i < table->capacity; i++) {
        handle_entry& entry = table->entries[i];
        if (entry.used) {
            continue;
        }

        resource_add_ref(obj);

        entry.used = true;
        entry.generation++;
        entry.flags = 0;
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
    resource_object** out_obj,
    uint32_t* out_flags,
    uint32_t* out_rights
) {
    if (!table || !out_obj) {
        return HANDLE_ERR_INVAL;
    }

    if (!handle_slot_in_range(table, handle)) {
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
    if (out_flags) {
        *out_flags = entry.flags;
    }
    if (out_rights) {
        *out_rights = entry.rights;
    }

    return HANDLE_OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t get_handle_flags(
    handle_table* table,
    handle_t handle,
    uint32_t* out_flags
) {
    if (!table || !out_flags) {
        return HANDLE_ERR_INVAL;
    }

    if (!handle_slot_in_range(table, handle)) {
        return HANDLE_ERR_NOENT;
    }

    sync::irq_lock_guard guard(table->lock);
    const handle_entry& entry = table->entries[static_cast<uint32_t>(handle)];
    if (!entry.used) {
        return HANDLE_ERR_NOENT;
    }

    *out_flags = entry.flags;
    return HANDLE_OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t set_handle_flags(
    handle_table* table,
    handle_t handle,
    uint32_t flags
) {
    if (!table) {
        return HANDLE_ERR_INVAL;
    }

    if (!handle_slot_in_range(table, handle)) {
        return HANDLE_ERR_NOENT;
    }

    sync::irq_lock_guard guard(table->lock);
    handle_entry& entry = table->entries[static_cast<uint32_t>(handle)];
    if (!entry.used) {
        return HANDLE_ERR_NOENT;
    }

    entry.flags = flags;
    return HANDLE_OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t install_handle_at(
    handle_table* table,
    handle_t slot,
    resource_object* obj,
    resource_type type,
    uint32_t rights
) {
    if (!table || !obj) {
        return HANDLE_ERR_INVAL;
    }

    if (!handle_slot_in_range(table, slot)) {
        return HANDLE_ERR_INVAL;
    }

    if (type == resource_type::UNKNOWN) {
        return HANDLE_ERR_INVAL;
    }

    resource_object* old_obj = nullptr;
    {
        sync::irq_lock_guard guard(table->lock);
        handle_entry& entry = table->entries[static_cast<uint32_t>(slot)];

        if (entry.used && entry.obj) {
            old_obj = entry.obj;
        }

        resource_add_ref(obj);

        entry.used = true;
        entry.generation++;
        entry.flags = 0;
        entry.rights = rights;
        entry.type = type;
        entry.obj = obj;
    }

    if (old_obj) {
        resource_release(old_obj);
    }

    return HANDLE_OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void inherit_standard_handles(handle_table* parent, handle_table* child) {
    for (handle_t slot = 0; slot < STANDARD_HANDLE_COUNT; slot++) {
        resource_object* obj = nullptr;
        uint32_t flags = 0;
        uint32_t rights = 0;
        if (get_handle_object(parent, slot, 0, &obj, &flags, &rights) != HANDLE_OK) {
            continue;
        }

        if (!(flags & RESOURCE_HANDLE_CLOEXEC)) {
            (void)install_handle_at(child, slot, obj, obj->type, rights);
        }
        resource_release(obj);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void copy_handle_table(handle_table* src, handle_table* dst) {
    sync::irq_lock_guard guard(src->lock);
    for (uint32_t i = 0; i < src->capacity; i++) {
        const handle_entry& entry = src->entries[i];
        if (!entry.used || !entry.obj) {
            continue;
        }

        dst->entries[i] = entry;
        resource_add_ref(entry.obj);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool handle_slot_in_range(const handle_table* table, handle_t slot) {
    return table && slot >= 0 && static_cast<uint32_t>(slot) < table->capacity;
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

    if (!handle_slot_in_range(table, handle)) {
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
    entry.flags = 0;

    *out_obj = obj;
    return HANDLE_OK;
}

} // namespace resource
