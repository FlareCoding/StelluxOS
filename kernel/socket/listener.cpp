#include "socket/listener.h"
#include "resource/resource.h"
#include "mm/heap.h"

namespace socket {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void listener_state::ref_destroy(listener_state* self) {
    if (!self) {
        return;
    }

    // No lock needed: refcount is 0, so no other thread has access.
    // socket_close already drained the queue and woke waiters before
    // dropping its ref. This handles the case where entries remain
    // (e.g., socket_node was the last ref holder after unlink).
    self->closed = true;
    while (pending_conn* pc = self->accept_queue.pop_front()) {
        self->pending_count--;
        resource::resource_release(pc->server_obj);
        heap::kfree(pc);
    }

    heap::kfree_delete(self);
}

} // namespace socket
