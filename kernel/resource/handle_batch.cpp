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

    batch->count = count;

    return batch;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void release_handle_batch(handle_batch* batch) {
    if (!batch) {
        return;
    }

    for (uint32_t i = 0; i < batch->count; i++) {
        resource_release(batch->entries[i].obj);
    }

    heap::kfree(batch);
}

} // namespace resource
