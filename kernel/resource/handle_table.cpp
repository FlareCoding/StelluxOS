#include "resource/handle_table.h"
#include "resource/resource.h"
#include "common/string.h"
#include "mm/heap.h"

namespace resource {

// Capacities double from INITIAL_TASK_HANDLES, so this is the smallest one holding slot `index`
static uint32_t capacity_for_slot(uint32_t index) {
    uint32_t capacity = INITIAL_TASK_HANDLES;
    while (capacity <= index) {
        capacity *= 2;
    }

    return capacity < MAX_TASK_HANDLES ? capacity : MAX_TASK_HANDLES;
}

/**
 * The entry for `slot`, or null when the table has not grown that far.
 * The caller must hold the table lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static handle_entry* entry_at(handle_table* table, handle_t slot) {
    auto index = static_cast<uint32_t>(slot);
    return index < table->capacity ? &table->entries[index] : nullptr;
}

/**
 * Grows `table` until it holds slot `index`, which must be below MAX_TASK_HANDLES.
 * The caller must hold the table lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t grow_to_hold(handle_table* table, uint32_t index) {
    uint32_t capacity = capacity_for_slot(index);
    if (capacity <= table->capacity) {
        return HANDLE_OK;
    }

    auto* grown = static_cast<handle_entry*>(heap::kzalloc(capacity * sizeof(handle_entry)));
    if (!grown) {
        return HANDLE_ERR_NOMEM;
    }

    string::memcpy(grown, table->entries, table->capacity * sizeof(handle_entry));
    heap::kfree(table->entries);
    table->entries = grown;
    table->capacity = capacity;
    return HANDLE_OK;
}

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
    auto* entries = static_cast<handle_entry*>(heap::kzalloc(INITIAL_TASK_HANDLES * sizeof(handle_entry)));
    if (!entries) {
        return HANDLE_ERR_NOMEM;
    }

    table->lock = sync::SPINLOCK_INIT;
    table->entries = entries;
    table->capacity = INITIAL_TASK_HANDLES;

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
    handle_t* out_handle,
    uint32_t limit
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

    // The lowest free slot, which is the first one past the end when every slot is taken
    uint32_t slot = 0;
    while (slot < table->capacity && table->entries[slot].used) {
        slot++;
    }

    if (slot >= limit || slot >= MAX_TASK_HANDLES) {
        return HANDLE_ERR_NOSPC;
    }

    int32_t rc = grow_to_hold(table, slot);
    if (rc != HANDLE_OK) {
        return rc;
    }

    resource_add_ref(obj);

    handle_entry& entry = table->entries[slot];
    entry.used = true;
    entry.generation++;
    entry.flags = 0;
    entry.rights = rights;
    entry.type = type;
    entry.obj = obj;

    *out_handle = static_cast<handle_t>(slot);
    return HANDLE_OK;
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

    if ((required_rights & ~RIGHT_MASK) != 0) {
        return HANDLE_ERR_INVAL;
    }

    sync::irq_lock_guard guard(table->lock);
    const handle_entry* entry = entry_at(table, handle);
    if (!entry || !entry->used || !entry->obj || entry->type == resource_type::UNKNOWN) {
        return HANDLE_ERR_NOENT;
    }

    if ((entry->rights & required_rights) != required_rights) {
        return HANDLE_ERR_ACCESS;
    }

    resource_add_ref(entry->obj);
    *out_obj = entry->obj;
    if (out_flags) {
        *out_flags = entry->flags;
    }
    if (out_rights) {
        *out_rights = entry->rights;
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

    sync::irq_lock_guard guard(table->lock);
    const handle_entry* entry = entry_at(table, handle);
    if (!entry || !entry->used) {
        return HANDLE_ERR_NOENT;
    }

    *out_flags = entry->flags;
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

    sync::irq_lock_guard guard(table->lock);
    handle_entry* entry = entry_at(table, handle);
    if (!entry || !entry->used) {
        return HANDLE_ERR_NOENT;
    }

    entry->flags = flags;
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
        int32_t rc = grow_to_hold(table, static_cast<uint32_t>(slot));
        if (rc != HANDLE_OK) {
            return rc;
        }

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
__PRIVILEGED_CODE int32_t copy_handle_table(handle_table* src, handle_table* dst) {
    sync::irq_lock_guard src_guard(src->lock);
    sync::irq_lock_guard dst_guard(dst->lock);
    int32_t rc = grow_to_hold(dst, src->capacity - 1);
    if (rc != HANDLE_OK) {
        return rc;
    }

    for (uint32_t i = 0; i < src->capacity; i++) {
        const handle_entry& entry = src->entries[i];
        if (!entry.used || !entry.obj) {
            continue;
        }

        dst->entries[i] = entry;
        resource_add_ref(entry.obj);
    }

    return HANDLE_OK;
}

bool handle_slot_in_range(const handle_table* table, handle_t slot) {
    return table && slot >= 0 && static_cast<uint32_t>(slot) < MAX_TASK_HANDLES;
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

    sync::irq_lock_guard guard(table->lock);
    handle_entry* entry = entry_at(table, handle);
    if (!entry || !entry->used || !entry->obj || entry->type == resource_type::UNKNOWN) {
        return HANDLE_ERR_NOENT;
    }

    resource_object* obj = entry->obj;
    entry->used = false;
    entry->rights = 0;
    entry->type = resource_type::UNKNOWN;
    entry->obj = nullptr;
    entry->flags = 0;

    *out_obj = obj;
    return HANDLE_OK;
}

} // namespace resource
