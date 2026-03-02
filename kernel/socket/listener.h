#ifndef STELLUX_SOCKET_LISTENER_H
#define STELLUX_SOCKET_LISTENER_H

#include "common/types.h"
#include "common/list.h"
#include "rc/ref_counted.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace resource { struct resource_object; }

namespace socket {

constexpr uint32_t DEFAULT_BACKLOG = 16;
constexpr uint32_t MAX_BACKLOG     = 32;

struct pending_conn {
    list::node link;
    resource::resource_object* server_obj;
};

struct listener_state : rc::ref_counted<listener_state> {
    sync::spinlock lock;
    bool closed;
    list::head<pending_conn, &pending_conn::link> accept_queue;
    sync::wait_queue accept_wq;
    uint32_t backlog;
    uint32_t pending_count;

    /**
     * Drains accept_queue, releases all pending server objects, frees self.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(listener_state* self);
};

} // namespace socket

#endif // STELLUX_SOCKET_LISTENER_H
