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

    heap::kfree(batch);
}

} // namespace resource
