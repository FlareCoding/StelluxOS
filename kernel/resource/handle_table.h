#ifndef STELLUX_RESOURCE_HANDLE_TABLE_H
#define STELLUX_RESOURCE_HANDLE_TABLE_H

#include "resource/resource_types.h"
#include "sync/spinlock.h"

namespace resource {

struct resource_object;

constexpr uint32_t MAX_TASK_HANDLES = 128;

struct handle_entry {
    bool used;
    uint16_t generation;
    uint32_t flags;
    uint32_t rights;
    resource_type type;
    resource_object* obj;
};

struct handle_table {
    sync::spinlock lock;
    handle_entry entries[MAX_TASK_HANDLES];
};

constexpr int32_t HANDLE_OK         = 0;
constexpr int32_t HANDLE_ERR_INVAL  = -1;
constexpr int32_t HANDLE_ERR_NOENT  = -2;
constexpr int32_t HANDLE_ERR_ACCESS = -3;
constexpr int32_t HANDLE_ERR_NOSPC  = -4;

/**
 * @brief Initialize a task's handle table.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_handle_table(handle_table* table);

/**
 * @brief Install a resource object and return new handle.
 * Increments object refcount on success.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t alloc_handle(
    handle_table* table,
    resource_object* obj,
    resource_type type,
    uint32_t rights,
    handle_t* out_handle
);

/**
 * @brief Resolve handle to resource object with rights check.
 * Increments object refcount on success; caller must release.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t get_handle_object(
    handle_table* table,
    handle_t handle,
    uint32_t required_rights,
    resource_object** out_obj,
    uint32_t* out_flags = nullptr
);

/**
 * @brief Get per-handle flags (O_NONBLOCK, etc.).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t get_handle_flags(
    handle_table* table,
    handle_t handle,
    uint32_t* out_flags
);

/**
 * @brief Set per-handle flags (O_NONBLOCK, etc.).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t set_handle_flags(
    handle_table* table,
    handle_t handle,
    uint32_t flags
);

/**
 * @brief Remove handle entry and return held object reference.
 * Does not release object; caller owns one reference on success.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t remove_handle(
    handle_table* table,
    handle_t handle,
    resource_object** out_obj
);

} // namespace resource

#endif // STELLUX_RESOURCE_HANDLE_TABLE_H
