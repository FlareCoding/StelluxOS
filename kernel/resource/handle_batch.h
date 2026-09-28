#ifndef STELLUX_RESOURCE_HANDLE_BATCH_H
#define STELLUX_RESOURCE_HANDLE_BATCH_H

#include "resource/resource.h"

namespace resource {

// Most handles one send may carry
constexpr uint32_t MAX_PASSED_HANDLES = 253;

// A handle on its way to another handle table: a reference to its object and the rights it grants
struct passed_handle {
    resource_object* obj;
    resource_type type;
    uint32_t rights;
};

// Handles sent together, holding their objects until a receiver installs them or they are dropped
struct handle_batch {
    uint32_t count;
    passed_handle entries[];
};

/**
 * @brief Allocates a batch of `count` empty entries.
 * @return The batch, or nullptr when `count` is zero, above MAX_PASSED_HANDLES, or memory runs out.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE handle_batch* create_handle_batch(uint32_t count);

/**
 * @brief Drops the reference each filled entry holds and frees the batch.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void release_handle_batch(handle_batch* batch);

} // namespace resource

#endif // STELLUX_RESOURCE_HANDLE_BATCH_H
