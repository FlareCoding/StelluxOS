#ifndef STELLUX_SOCKET_UNIX_SOCKET_H
#define STELLUX_SOCKET_UNIX_SOCKET_H

#include "common/types.h"
#include "rc/ref_counted.h"
#include "rc/strong_ref.h"
#include "sync/spinlock.h"
#include "sync/mutex.h"
#include "common/ring_buffer.h"
#include "socket/listener.h"
#include "resource/resource.h"
#include "resource/handle_batch.h"
#include "fs/node.h"

namespace socket {

constexpr uint32_t SOCK_STATE_UNBOUND   = 0;
constexpr uint32_t SOCK_STATE_BOUND     = 1;
constexpr uint32_t SOCK_STATE_LISTENING = 2;
constexpr uint32_t SOCK_STATE_CONNECTED = 3;

constexpr size_t UNIX_PATH_MAX = 108;

// The largest seqpacket message, sized so each direction's buffer takes exactly 64 KiB
constexpr size_t SEQPACKET_MAX_MESSAGE = 65535;

// Bounds the records that queued messages hold in each direction
constexpr size_t SEQPACKET_MAX_QUEUED_MESSAGES = 256;

// A unix socket carries a stream of bytes, or whole messages kept apart and in order over a connection
enum class unix_socket_type : uint8_t {
    stream,
    seqpacket,
};

// Handles sent with a stretch of the stream, taken by the receive that consumes its first byte
struct unix_record {
    ring_buffer_mark mark;
    resource::handle_batch* batch;
};

// One direction of a connected pair, carrying what one side writes to the other
struct unix_direction {
    ring_buffer* buf = nullptr;
};

struct unix_channel : rc::ref_counted<unix_channel> {
    unix_direction a_to_b;
    unix_direction b_to_a;

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(unix_channel* self);
};

struct unix_socket {
    uint32_t state;
    unix_socket_type type = unix_socket_type::stream;
    sync::spinlock lock;
    sync::mutex receive_lock;

    // The one queue pollers wait on in every state, since it lives as long as the socket
    sync::wait_queue poll_wq;

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
 * Create a connected socket pair of `type`.
 * On success, *out_a and *out_b each have refcount 1.
 * Caller must install handles and release the creation refs.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_socket_pair(
    resource::resource_object** out_a,
    resource::resource_object** out_b,
    unix_socket_type type = unix_socket_type::stream
);

/**
 * Create an unbound socket of `type`. Returns a resource_object with refcount 1.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_unbound_socket(
    resource::resource_object** out,
    unix_socket_type type = unix_socket_type::stream
);

/**
 * The ops table of unix sockets of `type`.
 */
const resource::resource_ops* get_socket_ops(unix_socket_type type);

} // namespace socket

#endif // STELLUX_SOCKET_UNIX_SOCKET_H
