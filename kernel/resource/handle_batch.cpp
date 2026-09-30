#include "resource/handle_batch.h"
#include "mm/heap.h"

namespace resource {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE handle_batch* create_handle_batch(uint32_t count) {
    if (count == 0 || count > MAX_PASSED_HANDLES) {
        return nullptr;
    }

    size_t size = sizeof(handle_batch) + count * sizeof(passed_handle);
    auto* batch = static_cast<handle_batch*>(heap::kzalloc(size));
    if (!batch) {
        return nullptr;
    }

    batch->ref_count.store_relaxed(1);
    batch->count = count;

    return batch;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool charge_handle_batch(handle_batch* batch, handle_table* table, uint32_t limit) {
    in_flight_account* account = table->in_flight.ptr();
    uint32_t in_flight = account->handles.load_relaxed();

    while (in_flight + batch->count <= limit) {
        if (account->handles.cmpxchg_weak_relaxed(in_flight, in_flight + batch->count)) {
            account->add_ref();
            batch->account = account;
            return true;
        }
    }

    return false;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void handle_batch_add_ref(handle_batch* batch) {
    batch->ref_count.fetch_add_relaxed(1);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void handle_batch_release(handle_batch* batch) {
    if (!batch || batch->ref_count.fetch_sub_acq_rel(1) != 1) {
        return;
    }

    for (uint32_t i = 0; i < batch->count; i++) {
        resource_release(batch->entries[i].obj);
    }

    if (batch->account) {
        batch->account->handles.fetch_sub_release(batch->count);
        if (batch->account->release()) {
            in_flight_account::ref_destroy(batch->account);
        }
    }

    heap::kfree(batch);
}

} // namespace resource
