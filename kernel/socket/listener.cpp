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

    // Refcount is 0 so no lock is needed. Entries can remain when the
    // socket_node held the last ref after unlink, so drain them here.
    self->closed = true;
    while (pending_conn* pc = self->accept_queue.pop_front()) {
        self->pending_count--;
        resource::resource_release(pc->server_obj);
        heap::kfree(pc);
    }

    heap::kfree_delete(self);
}

} // namespace socket
