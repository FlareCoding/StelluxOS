#ifndef STELLUX_RC_REAPER_H
#define STELLUX_RC_REAPER_H

#include "common/types.h"

namespace rc {
namespace reaper {

enum cleanup_result : uint32_t {
    DONE = 0,
    RETRY_LATER = 1
};

struct dead_node;
using cleanup_fn = cleanup_result (*)(dead_node*);

struct dead_node {
    dead_node* next;
    cleanup_fn cleanup;
    uint32_t queued;

    void init(cleanup_fn fn) {
        next = nullptr;
        cleanup = fn;
        queued = 0;
    }
};

constexpr uint32_t REAPER_BATCH_SIZE = 128;

constexpr int32_t OK         = 0;
constexpr int32_t ERR_NO_MEM = -1;

/**
 * Initialize the reaper subsystem and spawn the reaper task.
 * Call once during kernel bring-up, after sched::init().
 * @return OK on success, ERR_NO_MEM if task creation fails.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * Defer cleanup of a dead node.
 * Lock-free MPSC enqueue, safe from IRQ context and any CPU.
 * Wakes the reaper if queue transitions empty to non-empty.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void defer(dead_node* node);

/**
 * Adapter: converts a typed cleanup to cleanup_fn via member offset.
 */
template<typename T, cleanup_result (*Fn)(T*),
         dead_node T::*Member = &T::reaper_node>
inline cleanup_result reaper_thunk(dead_node* node) {
    if (!node) {
        return DONE;
    }
    const uintptr_t offset = reinterpret_cast<uintptr_t>(
        &(static_cast<T*>(nullptr)->*Member));
    T* object = reinterpret_cast<T*>(
        reinterpret_cast<uintptr_t>(node) - offset);
    return Fn(object);
}

} // namespace reaper
} // namespace rc

#endif // STELLUX_RC_REAPER_H
