#ifndef STELLUX_SOCKET_UNIX_SOCKET_H
#define STELLUX_SOCKET_UNIX_SOCKET_H

#include "common/types.h"
#include "rc/ref_counted.h"
#include "rc/strong_ref.h"
#include "sync/spinlock.h"
#include "socket/ring_buffer.h"
#include "socket/listener.h"
#include "resource/resource.h"
#include "fs/node.h"

namespace socket {

constexpr uint32_t SOCK_STATE_UNBOUND   = 0;
constexpr uint32_t SOCK_STATE_BOUND     = 1;
constexpr uint32_t SOCK_STATE_LISTENING = 2;
constexpr uint32_t SOCK_STATE_CONNECTED = 3;

constexpr size_t UNIX_PATH_MAX = 108;

struct unix_channel : rc::ref_counted<unix_channel> {
    ring_buffer* buf_a_to_b;
    ring_buffer* buf_b_to_a;

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(unix_channel* self);
};

struct unix_socket {
    uint32_t state;
    sync::spinlock lock;

    // CONNECTED state
    rc::strong_ref<unix_channel> channel;
    bool is_side_a;

    // BOUND / LISTENING state
    char bound_path[UNIX_PATH_MAX];
    rc::strong_ref<fs::node> bound_node;

    // LISTENING state
    rc::strong_ref<listener_state> listener;
};

/**
 * Create a connected socket pair.
 * On success, *out_a and *out_b each have refcount 1.
 * Caller must install handles and release the creation refs.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_socket_pair(
    resource::resource_object** out_a,
    resource::resource_object** out_b
);

/**
 * Create an unbound socket. Returns a resource_object with refcount 1.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_unbound_socket(
    resource::resource_object** out
);

/**
 * Access the global socket ops table (for connect server-side object creation).
 */
const resource::resource_ops* get_socket_ops();

} // namespace socket

#endif // STELLUX_SOCKET_UNIX_SOCKET_H
