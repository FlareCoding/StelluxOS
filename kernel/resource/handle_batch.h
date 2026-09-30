#ifndef STELLUX_RESOURCE_HANDLE_BATCH_H
#define STELLUX_RESOURCE_HANDLE_BATCH_H

#include "resource/resource.h"
#include "sync/atomic.h"

namespace resource {

// Most handles one send may carry
constexpr uint32_t MAX_PASSED_HANDLES = 253;

// A handle on its way to another handle table: a reference to its object and the rights it grants
struct passed_handle {
    resource_object* obj;
    resource_type type;
    uint32_t rights;
};

// Handles sent together, holding their objects until every holder of the batch has released it
struct handle_batch {
    sync::atomic<uint32_t> ref_count;
    uint32_t count;
    in_flight_account* account; // Charged for the entries until the batch is freed, or null
    passed_handle entries[];
};

/**
 * @brief Allocates a batch of `count` empty entries, with one reference owned by the caller.
 * @return The batch, or nullptr when `count` is zero, above MAX_PASSED_HANDLES, or memory runs out.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE handle_batch* create_handle_batch(uint32_t count);

/**
 * @brief Charges the batch's entries to the handles `table` has in flight, unless that would pass `limit`.
 * The batch refunds them when it is freed.
 * @return true when charged, false when the batch would take the table past its limit.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE bool charge_handle_batch(handle_batch* batch, handle_table* table, uint32_t limit);

/**
 * @brief Adds a reference to the batch, so a receive that only peeks can share a batch still queued.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void handle_batch_add_ref(handle_batch* batch);

/**
 * @brief Drops a reference to the batch. The last one releases each entry's object and frees the batch.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void handle_batch_release(handle_batch* batch);

} // namespace resource

#endif // STELLUX_RESOURCE_HANDLE_BATCH_H
