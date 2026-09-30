#include "socket/unix_socket.h"
#include "resource/socket_ops.h"
#include "resource/in_flight.h"
#include "net/inet.h"
#include "mm/heap.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "fs/fstypes.h"
#include "fs/fs.h"
#include "fs/socket_node.h"
#include "common/string.h"
#include "common/ring_buffer.h"
#include "sync/poll.h"
#include "hw/barrier.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"

namespace socket {

constexpr uint16_t AF_UNIX_VAL = 1;
constexpr size_t SUN_PATH_OFFSET = 2;

namespace {

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[UNIX_PATH_MAX];
};

// What a peek learns about the message it reached
struct message_peek {
    resource::handle_batch** out_batch;
    size_t length;
};

} // anonymous namespace

// Parse a kernel-copied sockaddr_un buffer into a validated path.
// Returns 0 on success, negative resource:: error on failure.
static int32_t parse_unix_addr(const void* kaddr, size_t addrlen,
                               char* kpath_out) {
    if (addrlen < sizeof(uint16_t) + 1) {
        return resource::ERR_INVAL;
    }

    size_t copy_len = addrlen < sizeof(sockaddr_un) ? addrlen : sizeof(sockaddr_un);

    sockaddr_un sa{};
    string::memcpy(&sa, kaddr, copy_len);

    if (sa.sun_family != AF_UNIX_VAL) {
        return resource::ERR_INVAL;
    }

    size_t path_max = copy_len - SUN_PATH_OFFSET;
    if (path_max == 0) {
        return resource::ERR_INVAL;
    }

    bool found_null = false;
    for (size_t i = 0; i < path_max; i++) {
        if (sa.sun_path[i] == '\0') {
            found_null = true;
            break;
        }
    }

    if (!found_null) return resource::ERR_INVAL;

    if (sa.sun_path[0] == '\0') return resource::ERR_INVAL;

    if (sa.sun_path[0] != '/') return resource::ERR_INVAL;

    string::memcpy(kpath_out, sa.sun_path, UNIX_PATH_MAX);
    return resource::OK;
}

static unix_record* record_of(ring_buffer_mark* mark) {
    uintptr_t offset = __builtin_offsetof(unix_record, mark);
    return reinterpret_cast<unix_record*>(reinterpret_cast<uintptr_t>(mark) - offset);
}

/**
 * Frees every record still queued on `rb` and drops the batches they carry.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void drop_records(ring_buffer* rb) {
    ring_buffer_mark_list marks;
    marks.init();
    ring_buffer_take_marks(rb, marks);

    while (ring_buffer_mark* mark = marks.pop_front()) {
        unix_record* record = record_of(mark);
        resource::handle_batch_release(record->batch);
        heap::kfree(record);
    }
}

__PRIVILEGED_CODE void unix_channel::ref_destroy(unix_channel* self) {
    if (!self) {
        return;
    }

    drop_records(self->a_to_b.buf);
    drop_records(self->b_to_a.buf);
    ring_buffer_destroy(self->a_to_b.buf);
    ring_buffer_destroy(self->b_to_a.buf);
    heap::kfree_delete(self);
}

/**
 * The buffer for one direction of a channel between sockets of `type`.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ring_buffer* create_direction_buffer(unix_socket_type type) {
    if (type == unix_socket_type::seqpacket) {
        return ring_buffer_create(SEQPACKET_MAX_MESSAGE, SEQPACKET_MAX_QUEUED_MESSAGES);
    }

    return ring_buffer_create(RING_BUFFER_DEFAULT_CAPACITY);
}

/**
 * A channel between sockets of `type` with a buffer for each direction, or null when memory runs out.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static rc::strong_ref<unix_channel> create_channel(unix_socket_type type) {
    auto chan = rc::make_kref<unix_channel>();
    if (!chan) {
        return rc::strong_ref<unix_channel>();
    }

    chan->a_to_b.buf = create_direction_buffer(type);
    if (!chan->a_to_b.buf) {
        return rc::strong_ref<unix_channel>();
    }

    chan->b_to_a.buf = create_direction_buffer(type);
    if (!chan->b_to_a.buf) {
        return rc::strong_ref<unix_channel>();
    }

    return chan;
}

/**
 * The direction `sock` reads from.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static unix_direction& inbound(const unix_socket* sock) {
    return sock->is_side_a ? sock->channel->b_to_a : sock->channel->a_to_b;
}

/**
 * The direction `sock` writes to.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static unix_direction& outbound(const unix_socket* sock) {
    return sock->is_side_a ? sock->channel->a_to_b : sock->channel->b_to_a;
}

/**
 * Lets the channel's buffers wake whoever polls `sock`.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void attach_poll_queue(unix_socket* sock) {
    ring_buffer_set_reader_poll_queue(inbound(sock).buf, &sock->poll_wq);
    ring_buffer_set_writer_poll_queue(outbound(sock).buf, &sock->poll_wq);
}

/**
 * Keeps the channel's buffers, which the peer may hold on to, from reaching a socket about to be freed.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void detach_poll_queue(unix_socket* sock) {
    ring_buffer_set_reader_poll_queue(inbound(sock).buf, nullptr);
    ring_buffer_set_writer_poll_queue(outbound(sock).buf, nullptr);
}

/**
 * Writes every byte with `batch` on the first stretch, waiting for room unless MSG_DONTWAIT. A later error
 * reports the partial count, and a reader gone before any byte went out raises SIGPIPE unless MSG_NOSIGNAL.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t write_stream(unix_direction& dir, const uint8_t* bytes, size_t count,
                                              uint32_t msg_flags, resource::handle_batch* batch) {
    unix_record* unsent_record = nullptr;
    if (batch && count > 0) {
        unsent_record = static_cast<unix_record*>(heap::kzalloc(sizeof(unix_record)));
        if (!unsent_record) {
            return resource::ERR_NOMEM;
        }

        unsent_record->batch = batch;
    }

    bool nonblock = (msg_flags & net::inet::MSG_DONTWAIT) != 0;
    size_t sent = 0;
    ssize_t n = 0;

    if (count == 0 && (ring_buffer_poll_write(dir.buf, nullptr) & sync::POLL_ERR)) {
        n = RB_ERR_PIPE;
    }

    while (sent < count) {
        ring_buffer_mark* mark = unsent_record ? &unsent_record->mark : nullptr;
        n = ring_buffer_write_marked(dir.buf, bytes + sent, count - sent, mark, nonblock);
        if (n < 0) {
            break;
        }

        unsent_record = nullptr;
        sent += static_cast<size_t>(n);
    }

    if (unsent_record) {
        heap::kfree(unsent_record);
    }

    if (sent > 0) {
        return static_cast<ssize_t>(sent);
    }

    if (n == RB_ERR_PIPE && !(msg_flags & net::inet::MSG_NOSIGNAL)) {
        (void)signals::send_to_task(sched::current(), signals::SIGPIPE);
    }

    return n;
}

/**
 * Writes all `count` bytes as one message carrying `batch`, waiting for room unless MSG_DONTWAIT. A message
 * of no bytes is not sent, and a reader gone raises SIGPIPE unless MSG_NOSIGNAL.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t write_message(unix_direction& dir, const uint8_t* bytes, size_t count,
                                               uint32_t msg_flags, resource::handle_batch* batch) {
    ssize_t n = 0;
    if (count == 0) {
        n = (ring_buffer_poll_write(dir.buf, nullptr) & sync::POLL_ERR) ? RB_ERR_PIPE : 0;
    } else {
        auto* record = static_cast<unix_record*>(heap::kzalloc(sizeof(unix_record)));
        if (!record) {
            return resource::ERR_NOMEM;
        }

        record->batch = batch;
        bool nonblock = (msg_flags & net::inet::MSG_DONTWAIT) != 0;
        n = ring_buffer_write_all_marked(dir.buf, bytes, count, &record->mark, nonblock);
        if (n < 0) {
            heap::kfree(record);
        }
    }

    if (n == RB_ERR_PIPE && !(msg_flags & net::inet::MSG_NOSIGNAL)) {
        (void)signals::send_to_task(sched::current(), signals::SIGPIPE);
    }

    return n;
}

/**
 * Writes to the peer as part of a stream or as one message, by the socket's type.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t write_to_peer(unix_socket* sock, const uint8_t* bytes, size_t count,
                                               uint32_t msg_flags, resource::handle_batch* batch) {
    if (sock->type == unix_socket_type::seqpacket) {
        return write_message(outbound(sock), bytes, count, msg_flags, batch);
    }

    return write_stream(outbound(sock), bytes, count, msg_flags, batch);
}

/**
 * Shares the batch of the stretch a peek reached, if it carries one, running under the buffer's lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void share_batch(ring_buffer_mark* mark, void* out_batch) {
    resource::handle_batch* batch = record_of(mark)->batch;
    if (batch) {
        resource::handle_batch_add_ref(batch);
    }

    *static_cast<resource::handle_batch**>(out_batch) = batch;
}

/**
 * Receives what is queued without waiting, stopping where a stretch that carries a batch ends. Given
 * `out_batch`, the receive consuming the stretch's first byte takes its batch and a peek shares it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t receive_queued(unix_socket* sock, uint8_t* dst, size_t count, bool peek,
                                                resource::handle_batch** out_batch) {
    ring_buffer* rb = inbound(sock).buf;

    if (peek) {
        return ring_buffer_peek_marked(rb, dst, count, out_batch ? share_batch : nullptr, out_batch);
    }

    ring_buffer_mark* taken = nullptr;
    ssize_t n = ring_buffer_read_marked(rb, dst, count, &taken);

    if (taken) {
        unix_record* record = record_of(taken);

        if (out_batch) {
            *out_batch = record->batch;
        } else {
            resource::handle_batch_release(record->batch);
        }

        heap::kfree(record);
    }

    return n;
}

/**
 * Receives like receive_queued, waiting for bytes unless MSG_DONTWAIT.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t receive_or_wait(unix_socket* sock, uint8_t* dst, size_t count, uint32_t flags,
                                                 resource::handle_batch** out_batch) {
    if (count == 0) {
        return 0;
    }

    bool nonblock = (flags & net::inet::MSG_DONTWAIT) != 0;
    bool peek = (flags & net::inet::MSG_PEEK) != 0;

    while (true) {
        ssize_t n = receive_queued(sock, dst, count, peek, out_batch);
        if (n != RB_ERR_AGAIN || nonblock) {
            return n;
        }

        ssize_t queued = ring_buffer_wait_readable(inbound(sock).buf, false);
        if (queued <= 0) {
            return queued;
        }
    }
}

/**
 * Notes the length of the message a peek reached and shares its batch when asked, running under the
 * buffer's lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void note_peeked_message(ring_buffer_mark* mark, void* peek_state) {
    auto* peeked = static_cast<message_peek*>(peek_state);
    peeked->length = mark->length;

    if (peeked->out_batch) {
        share_batch(mark, peeked->out_batch);
    }
}

/**
 * Takes the message at the front whole, copying what fits of it into `dst`, or only looks at it when
 * peeking. Given `out_batch`, a receive takes the message's batch and a peek shares it. The caller must
 * hold receive_lock, since a message that does not fit leaves the buffer in two reads.
 * @return The message's full length, 0 at end of stream, or RB_ERR_AGAIN when none is queued.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t take_queued_message(unix_socket* sock, uint8_t* dst, size_t count, bool peek,
                                                     resource::handle_batch** out_batch) {
    ring_buffer* rb = inbound(sock).buf;

    // The buffer never reads zero bytes, so a receive with no room takes the message without copying it
    uint8_t* copy_to = count > 0 ? dst : nullptr;
    size_t copy_limit = count > 0 ? count : SEQPACKET_MAX_MESSAGE;

    if (peek) {
        message_peek peeked = {out_batch, 0};
        ssize_t n = ring_buffer_peek_marked(rb, copy_to, copy_limit, note_peeked_message, &peeked);
        return n > 0 ? static_cast<ssize_t>(peeked.length) : n;
    }

    ring_buffer_mark* taken = nullptr;
    ssize_t n = ring_buffer_read_marked(rb, copy_to, copy_limit, &taken);
    if (n <= 0) {
        return n;
    }

    size_t length = taken->length;
    if (length > static_cast<size_t>(n)) {
        ring_buffer_mark* none = nullptr;
        (void)ring_buffer_read_marked(rb, nullptr, length - static_cast<size_t>(n), &none);
    }

    unix_record* record = record_of(taken);
    if (out_batch) {
        *out_batch = record->batch;
    } else {
        resource::handle_batch_release(record->batch);
    }

    heap::kfree(record);

    return static_cast<ssize_t>(length);
}

/**
 * Receives one message like take_queued_message, waiting for one unless MSG_DONTWAIT. Holds receive_lock
 * except while waiting, so a sleeping receiver holds up no other.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t receive_message(unix_socket* sock, uint8_t* dst, size_t count, uint32_t flags,
                                                 resource::handle_batch** out_batch) {
    bool nonblock = (flags & net::inet::MSG_DONTWAIT) != 0;
    bool peek = (flags & net::inet::MSG_PEEK) != 0;

    sync::mutex_lock(sock->receive_lock);

    ssize_t n = take_queued_message(sock, dst, count, peek, out_batch);
    while (n == RB_ERR_AGAIN && !nonblock) {
        sync::mutex_unlock(sock->receive_lock);
        ssize_t queued = ring_buffer_wait_readable(inbound(sock).buf, false);
        sync::mutex_lock(sock->receive_lock);

        n = queued > 0 ? take_queued_message(sock, dst, count, peek, out_batch) : queued;
    }

    sync::mutex_unlock(sock->receive_lock);

    return n;
}

/**
 * Never waits holding receive_lock, so a sleeping reader holds up no other. A read takes no
 * handles, so it drops the batch of a stretch it starts.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t read_stream(unix_socket* sock, uint8_t* bytes, size_t count, bool nonblock) {
    if (count == 0) {
        return 0;
    }

    resource::handle_batch* batch = nullptr;
    sync::mutex_lock(sock->receive_lock);
    ssize_t n = receive_queued(sock, bytes, count, false, &batch);

    while (n == RB_ERR_AGAIN && !nonblock) {
        sync::mutex_unlock(sock->receive_lock);
        ssize_t queued = ring_buffer_wait_readable(inbound(sock).buf, false);
        sync::mutex_lock(sock->receive_lock);

        n = queued > 0 ? receive_queued(sock, bytes, count, false, &batch) : queued;
    }

    sync::mutex_unlock(sock->receive_lock);
    resource::handle_batch_release(batch);

    return n;
}

__PRIVILEGED_CODE static ssize_t socket_read(
    resource::resource_object* obj, void* kdst, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !kdst) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (sock->state != SOCK_STATE_CONNECTED) {
        return resource::ERR_NOTCONN;
    }

    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    if (sock->type == unix_socket_type::stream) {
        return read_stream(sock, static_cast<uint8_t*>(kdst), count, nonblock);
    }

    // A plain read takes no handles and reports only the bytes it copied
    uint32_t msg_flags = nonblock ? net::inet::MSG_DONTWAIT : 0;
    ssize_t length = receive_message(sock, static_cast<uint8_t*>(kdst), count, msg_flags, nullptr);

    return length > static_cast<ssize_t>(count) ? static_cast<ssize_t>(count) : length;
}

__PRIVILEGED_CODE static ssize_t socket_write(
    resource::resource_object* obj, const void* ksrc, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !ksrc) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (sock->state != SOCK_STATE_CONNECTED) {
        return resource::ERR_NOTCONN;
    }

    uint32_t msg_flags = (flags & fs::O_NONBLOCK) ? net::inet::MSG_DONTWAIT : 0;
    return write_to_peer(sock, static_cast<const uint8_t*>(ksrc), count, msg_flags, nullptr);
}

/**
 * Closes the reading end of `dir` for good and drops the batches no receive can reach anymore.
 * Closing first leaves no write that could still add one.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void close_reading_end(unix_direction& dir) {
    ring_buffer_close_read(dir.buf);
    drop_records(dir.buf);
}

__PRIVILEGED_CODE static void socket_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    auto* sock = static_cast<unix_socket*>(obj->impl);

    switch (sock->state) {
    case SOCK_STATE_UNBOUND:
        break;

    case SOCK_STATE_BOUND:
        // bound_node strong_ref drops via ~unix_socket
        break;

    case SOCK_STATE_LISTENING: {
        if (sock->listener) {
            list::head<pending_conn, &pending_conn::link> refused;
            refused.init();

            sync::irq_state irq = sync::spin_lock_irqsave(sock->listener->lock);
            sock->listener->closed = true;
            sock->listener->poll_wq = nullptr;
            while (pending_conn* pc = sock->listener->accept_queue.pop_front()) {
                sock->listener->pending_count--;
                refused.push_back(pc);
            }
            sync::spin_unlock_irqrestore(sock->listener->lock, irq);
            sync::wake_all(sock->listener->accept_wq);

            // Released outside the spinlock, since a connection's queued handles may sleep while closing
            while (pending_conn* pc = refused.pop_front()) {
                resource::resource_release(pc->server_obj);
                heap::kfree(pc);
            }
        }
        // bound_node and listener strong_refs drop via ~unix_socket
        break;
    }

    case SOCK_STATE_CONNECTED: {
        if (sock->channel) {
            detach_poll_queue(sock);
            ring_buffer_close_write(outbound(sock).buf);
            close_reading_end(inbound(sock));
        }
        break;
    }
    }

    heap::kfree_delete(sock);
    obj->impl = nullptr;
}

__PRIVILEGED_CODE static int32_t unix_bind(
    resource::resource_object* obj, const void* kaddr, size_t addrlen
) {
    if (!obj || !obj->impl) return resource::ERR_INVAL;

    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (sock->state != SOCK_STATE_UNBOUND) {
        return resource::ERR_INVAL;
    }

    char kpath[UNIX_PATH_MAX];
    int32_t rc = parse_unix_addr(kaddr, addrlen, kpath);
    if (rc != resource::OK) return rc;

    fs::node* parent = nullptr;
    const char* name = nullptr;
    size_t name_len = 0;
    rc = fs::resolve_parent_path(kpath, &parent, &name, &name_len);
    if (rc != fs::OK) {
        if (rc == fs::ERR_NOENT) return resource::ERR_NOENT;
        if (rc == fs::ERR_NOTDIR) return resource::ERR_NOTDIR;
        return resource::ERR_INVAL;
    }

    fs::node* sock_node = nullptr;
    rc = parent->create_socket(name, name_len, nullptr, &sock_node);
    if (parent->release()) {
        fs::node::ref_destroy(parent);
    }
    if (rc != fs::OK) {
        if (rc == fs::ERR_EXIST) return resource::ERR_ADDRINUSE;
        if (rc == fs::ERR_NOMEM) return resource::ERR_NOMEM;
        return resource::ERR_IO;
    }

    string::memcpy(sock->bound_path, kpath, UNIX_PATH_MAX);
    sock->bound_node = rc::strong_ref<fs::node>::adopt(sock_node);
    sock->state = SOCK_STATE_BOUND;
    return resource::OK;
}

__PRIVILEGED_CODE static int32_t unix_listen(
    resource::resource_object* obj, int32_t backlog
) {
    if (!obj || !obj->impl) return resource::ERR_INVAL;

    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (sock->state != SOCK_STATE_BOUND) {
        return resource::ERR_INVAL;
    }

    auto ls = rc::make_kref<listener_state>();
    if (!ls) {
        return resource::ERR_NOMEM;
    }

    ls->lock = sync::SPINLOCK_INIT;
    ls->closed = false;
    ls->type = sock->type;
    ls->accept_queue.init();
    ls->accept_wq.init();

    uint32_t bl = (backlog <= 0) ? DEFAULT_BACKLOG : static_cast<uint32_t>(backlog);
    if (bl > MAX_BACKLOG) {
        bl = MAX_BACKLOG;
    }
    ls->backlog = bl;
    ls->pending_count = 0;
    ls->poll_wq = &sock->poll_wq;

    // A connect on another CPU finds the listener through the node, so every field must be visible first
    barrier::smp_write();
    sock->listener = ls;

    if (sock->bound_node) {
        auto* sn = static_cast<fs::socket_node*>(sock->bound_node.ptr());
        rc::strong_ref<listener_state> ls_copy = ls;
        sn->set_listener(static_cast<rc::strong_ref<listener_state>&&>(ls_copy));
    }

    sock->state = SOCK_STATE_LISTENING;
    return resource::OK;
}

__PRIVILEGED_CODE static int32_t unix_accept(
    resource::resource_object* obj, resource::resource_object** new_obj,
    void* kaddr, size_t* addrlen, bool nonblock
) {
    if (!obj || !obj->impl || !new_obj) return resource::ERR_INVAL;

    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (sock->state != SOCK_STATE_LISTENING || !sock->listener) {
        return resource::ERR_INVAL;
    }

    sched::task* task = sched::current();
    if (!task) return resource::ERR_IO;

    listener_state* ls = sock->listener.ptr();

    sync::irq_state irq = sync::spin_lock_irqsave(ls->lock);
    if (ls->accept_queue.empty()) {
        if (ls->closed) {
            sync::spin_unlock_irqrestore(ls->lock, irq);
            return resource::ERR_INVAL;
        }

        if (nonblock) {
            sync::spin_unlock_irqrestore(ls->lock, irq);
            return resource::ERR_AGAIN;
        }
    }

    while (ls->accept_queue.empty() && !ls->closed
           && !signals::interrupt_pending(task)) {
        irq = sync::wait(ls->accept_wq, ls->lock, irq);
    }

    if (signals::interrupt_pending(task)) {
        sync::spin_unlock_irqrestore(ls->lock, irq);
        return resource::ERR_INTR;
    }

    if (ls->accept_queue.empty()) {
        sync::spin_unlock_irqrestore(ls->lock, irq);
        return resource::ERR_INVAL;
    }

    pending_conn* pc = ls->accept_queue.pop_front();
    ls->pending_count--;
    sync::spin_unlock_irqrestore(ls->lock, irq);

    *new_obj = pc->server_obj;
    heap::kfree(pc);

    if (kaddr && addrlen && *addrlen >= sizeof(uint16_t)) {
        uint16_t family = AF_UNIX_VAL;
        string::memcpy(kaddr, &family, sizeof(family));
        *addrlen = sizeof(uint16_t);
    }

    return resource::OK;
}

__PRIVILEGED_CODE static int32_t unix_connect(
    resource::resource_object* obj, const void* kaddr, size_t addrlen, bool
) {
    if (!obj || !obj->impl) {
        return resource::ERR_INVAL;
    }

    auto* client_sock = static_cast<unix_socket*>(obj->impl);
    if (client_sock->state == SOCK_STATE_CONNECTED) {
        return resource::ERR_ISCONN;
    }

    if (client_sock->state == SOCK_STATE_LISTENING) {
        return resource::ERR_INVAL;
    }

    char kpath[UNIX_PATH_MAX];
    int32_t rc = parse_unix_addr(kaddr, addrlen, kpath);
    if (rc != resource::OK) {
        return rc;
    }

    fs::node* target_node = nullptr;
    rc = fs::lookup(kpath, &target_node);
    if (rc != fs::OK) {
        if (rc == fs::ERR_NOENT) {
            return resource::ERR_NOENT;
        }

        return resource::ERR_CONNREFUSED;
    }

    if (target_node->type() != fs::node_type::socket) {
        if (target_node->release()) {
            fs::node::ref_destroy(target_node);
        }

        return resource::ERR_CONNREFUSED;
    }

    auto* sn = static_cast<fs::socket_node*>(target_node);
    listener_state* raw_ls = sn->get_listener();
    if (!raw_ls) {
        if (target_node->release()) {
            fs::node::ref_destroy(target_node);
        }
        return resource::ERR_CONNREFUSED;
    }

    rc::strong_ref<listener_state> ls_ref =
        rc::strong_ref<listener_state>::try_from_raw(raw_ls);
    if (target_node->release()) {
        fs::node::ref_destroy(target_node);
    }

    if (!ls_ref) {
        return resource::ERR_CONNREFUSED;
    }

    if (ls_ref->type != client_sock->type) {
        return resource::ERR_PROTOTYPE;
    }

    auto chan = create_channel(client_sock->type);
    if (!chan) {
        return resource::ERR_NOMEM;
    }

    auto* server_sock = heap::kalloc_new<unix_socket>();
    if (!server_sock) {
        return resource::ERR_NOMEM;
    }

    server_sock->state = SOCK_STATE_CONNECTED;
    server_sock->type = client_sock->type;
    server_sock->lock = sync::SPINLOCK_INIT;
    server_sock->receive_lock.init();
    server_sock->poll_wq.init();
    server_sock->is_side_a = true;
    server_sock->channel = chan;
    attach_poll_queue(server_sock);

    auto* server_obj = heap::kalloc_new<resource::resource_object>();
    if (!server_obj) {
        heap::kfree_delete(server_sock);
        return resource::ERR_NOMEM;
    }

    server_obj->type = resource::resource_type::SOCKET;
    server_obj->ops = get_socket_ops(client_sock->type);
    server_obj->impl = server_sock;

    auto* pc = static_cast<pending_conn*>(
        heap::kzalloc(sizeof(pending_conn)));
    if (!pc) {
        heap::kfree_delete(server_obj);
        heap::kfree_delete(server_sock);
        return resource::ERR_NOMEM;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(ls_ref->lock);
    if (ls_ref->closed || ls_ref->pending_count >= ls_ref->backlog) {
        sync::spin_unlock_irqrestore(ls_ref->lock, irq);
        heap::kfree(pc);
        heap::kfree_delete(server_obj);
        heap::kfree_delete(server_sock);
        return resource::ERR_CONNREFUSED;
    }

    // Attached before the server end can be accepted, so none of its writes goes unannounced
    client_sock->channel = static_cast<rc::strong_ref<unix_channel>&&>(chan);
    client_sock->is_side_a = false;

    attach_poll_queue(client_sock);
    barrier::smp_write();
    client_sock->state = SOCK_STATE_CONNECTED;

    pc->server_obj = server_obj;
    ls_ref->accept_queue.push_back(pc);
    ls_ref->pending_count++;

    if (ls_ref->poll_wq) {
        sync::wake_all(*ls_ref->poll_wq);
    }

    sync::spin_unlock_irqrestore(ls_ref->lock, irq);
    sync::wake_one(ls_ref->accept_wq);

    // Pollers that began before the connect see the socket connected
    sync::wake_all(client_sock->poll_wq);

    return resource::OK;
}

static bool is_unix_socket(const resource::resource_object* obj) {
    return obj->ops == get_socket_ops(unix_socket_type::stream) ||
           obj->ops == get_socket_ops(unix_socket_type::seqpacket);
}

/**
 * Whether `obj` is the connected socket that reads from the buffer `context` names.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool reads_from(resource::resource_object* obj, void* context) {
    if (!is_unix_socket(obj) || !obj->impl) {
        return false;
    }

    auto* sock = static_cast<unix_socket*>(obj->impl);

    return sock->state == SOCK_STATE_CONNECTED && inbound(sock).buf == context;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void visit_record_batch(ring_buffer_mark* mark, void* walk) {
    resource::handle_batch* batch = record_of(mark)->batch;
    if (!batch) {
        return;
    }

    for (uint32_t i = 0; i < batch->count; i++) {
        resource::in_flight_visit(*static_cast<resource::in_flight_walk*>(walk), batch->entries[i].obj);
    }
}

/**
 * Reports what is queued for `obj` to read and, while it listens, the connections waiting to be accepted.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void unix_visit_held(resource::resource_object* obj, resource::in_flight_walk& walk) {
    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (!sock) {
        return;
    }

    if (sock->state == SOCK_STATE_CONNECTED) {
        ring_buffer_for_each_mark(inbound(sock).buf, visit_record_batch, &walk);
        return;
    }

    if (sock->state == SOCK_STATE_LISTENING && sock->listener) {
        listener_state* ls = sock->listener.ptr();

        sync::irq_state irq = sync::spin_lock_irqsave(ls->lock);
        for (pending_conn& pc : ls->accept_queue) {
            resource::in_flight_visit(walk, pc.server_obj);
        }

        sync::spin_unlock_irqrestore(ls->lock, irq);
    }
}

static bool carries_holder(const resource::handle_batch* batch) {
    for (uint32_t i = 0; i < batch->count; i++) {
        if (batch->entries[i].obj->ops->visit_held) {
            return true;
        }
    }

    return false;
}

/**
 * Writes like write_to_peer for a batch that carries holders, queuing it only after checking under the
 * in-flight lock that nothing in it holds the reading socket, so no reference loop can form. Room is
 * waited for with the lock dropped, and the check is redone after each wait.
 * @return Bytes written, ERR_LOOP when the batch holds the reader, or another write error.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t write_checked(unix_socket* sock, const uint8_t* bytes, size_t count,
                                               uint32_t msg_flags, resource::handle_batch* batch) {
    unix_direction& dir = outbound(sock);
    bool nonblock = (msg_flags & net::inet::MSG_DONTWAIT) != 0;
    size_t first_write = sock->type == unix_socket_type::seqpacket ? count : 1;
    ssize_t n = 0;

    while (true) {
        sync::mutex_lock(resource::in_flight_lock());
        bool loops = resource::in_flight_reaches(batch->entries, batch->count, reads_from, dir.buf);
        n = loops ? resource::ERR_LOOP
                  : write_to_peer(sock, bytes, count, msg_flags | net::inet::MSG_DONTWAIT, batch);
        sync::mutex_unlock(resource::in_flight_lock());

        if (n != resource::ERR_AGAIN || nonblock) {
            break;
        }

        // A closed reader is reported by the write itself, along with its signal
        if (ring_buffer_wait_writable(dir.buf, first_write, true, false) == RB_ERR_INTR) {
            return resource::ERR_INTR;
        }
    }

    if (n <= 0 || nonblock || static_cast<size_t>(n) == count) {
        return n;
    }

    // The batch went with the first stretch, the rest of the stream follows at the caller's pace
    size_t sent = static_cast<size_t>(n);
    ssize_t more = write_stream(dir, bytes + sent, count - sent, msg_flags, nullptr);

    return static_cast<ssize_t>(more > 0 ? sent + static_cast<size_t>(more) : sent);
}

/**
 * One to MAX_PASSED_HANDLES filled entries.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t check_batch(const resource::handle_batch* batch) {
    if (!batch || batch->count == 0 || batch->count > resource::MAX_PASSED_HANDLES) {
        return resource::ERR_INVAL;
    }

    for (uint32_t i = 0; i < batch->count; i++) {
        if (!batch->entries[i].obj) {
            return resource::ERR_INVAL;
        }
    }

    return resource::OK;
}

/**
 * A connected stream takes no destination, and out of band data is not supported.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t unix_sendmsg(
    resource::resource_object* obj, const void* ksrc, size_t count, uint32_t flags, const void*, size_t addrlen,
    resource::handle_batch* batch
) {
    if (!obj || !obj->impl || !ksrc) {
        return resource::ERR_INVAL;
    }

    if (flags & net::inet::MSG_OOB) {
        return resource::ERR_UNSUP;
    }

    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (addrlen != 0) {
        return sock->state == SOCK_STATE_CONNECTED ? resource::ERR_ISCONN : resource::ERR_UNSUP;
    }

    if (sock->state != SOCK_STATE_CONNECTED) {
        return resource::ERR_NOTCONN;
    }

    int32_t rc = batch ? check_batch(batch) : resource::OK;
    if (rc != resource::OK) {
        return rc;
    }

    if (batch && count > 0 && carries_holder(batch)) {
        return write_checked(sock, static_cast<const uint8_t*>(ksrc), count, flags, batch);
    }

    return write_to_peer(sock, static_cast<const uint8_t*>(ksrc), count, flags, batch);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t unix_sendto(
    resource::resource_object* obj, const void* ksrc, size_t count, uint32_t flags, const void* kaddr, size_t addrlen
) {
    return unix_sendmsg(obj, ksrc, count, flags, kaddr, addrlen, nullptr);
}

/**
 * A null destination discards the bytes instead of copying them, and a peek leaves them
 * queued, even a discarding one. The peer of a pair has no address to report.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t unix_recvmsg(
    resource::resource_object* obj, void* kdst, size_t count, uint32_t flags, void*, size_t* addrlen,
    resource::handle_batch** out_batch
) {
    if (!obj || !obj->impl) {
        return resource::ERR_INVAL;
    }

    if (out_batch) {
        *out_batch = nullptr;
    }

    if (flags & net::inet::MSG_OOB) {
        return resource::ERR_UNSUP;
    }

    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (sock->state != SOCK_STATE_CONNECTED) {
        return resource::ERR_NOTCONN;
    }

    if (addrlen) {
        *addrlen = 0;
    }

    if (sock->type == unix_socket_type::seqpacket) {
        return receive_message(sock, static_cast<uint8_t*>(kdst), count, flags, out_batch);
    }

    return receive_or_wait(sock, static_cast<uint8_t*>(kdst), count, flags, out_batch);
}

/**
 * A plain receive takes no handles, so it drops the batch of a stretch it consumes.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t unix_recvfrom(
    resource::resource_object* obj, void* kdst, size_t count, uint32_t flags, void* kaddr, size_t* addrlen
) {
    return unix_recvmsg(obj, kdst, count, flags, kaddr, addrlen, nullptr);
}

/**
 * Lives on the socket rather than its connection, so a receive racing a connect still takes it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static sync::mutex* unix_receive_lock(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return nullptr;
    }

    return &static_cast<unix_socket*>(obj->impl)->receive_lock;
}

/**
 * An ended direction never makes a caller wait: an ended read side polls readable with POLL_RDHUP,
 * an ended write side polls writable, and the stream hangs up only once both have ended.
 */
static uint32_t stream_events(uint32_t in, uint32_t out) {
    bool reads_over = (in & sync::POLL_HUP) != 0;
    bool writes_over = (out & sync::POLL_ERR) != 0;
    uint32_t events = (in & sync::POLL_IN) | (out & sync::POLL_OUT);

    if (reads_over) {
        events |= sync::POLL_IN | sync::POLL_RDHUP;
    }

    if (writes_over) {
        events |= sync::POLL_OUT;
    }

    if (reads_over && writes_over) {
        events |= sync::POLL_HUP;
    }

    return events;
}

/**
 * Shutting the read side ends this end's stream and refuses the peer's sends. Shutting
 * the write side lets the peer drain what was sent and then see the end of the stream.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t unix_shutdown(resource::resource_object* obj, int32_t how) {
    if (!obj || !obj->impl) {
        return resource::ERR_INVAL;
    }

    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (sock->state != SOCK_STATE_CONNECTED) {
        return resource::ERR_NOTCONN;
    }

    if (how == resource::SHUT_RD || how == resource::SHUT_RDWR) {
        ring_buffer_close_read(inbound(sock).buf);
    }

    if (how == resource::SHUT_WR || how == resource::SHUT_RDWR) {
        ring_buffer_close_write(outbound(sock).buf);
    }

    return resource::OK;
}

__PRIVILEGED_CODE static uint32_t socket_poll(
    resource::resource_object* obj, sync::poll_table* pt
) {
    if (!obj || !obj->impl) {
        return sync::POLL_NVAL;
    }

    auto* sock = static_cast<unix_socket*>(obj->impl);
    if (pt) {
        sync::poll_subscribe(*pt, sock->poll_wq);
    }

    if (sock->state == SOCK_STATE_CONNECTED) {
        return stream_events(ring_buffer_poll_read(inbound(sock).buf, nullptr),
                             ring_buffer_poll_write(outbound(sock).buf, nullptr));
    }

    if (sock->state == SOCK_STATE_LISTENING && sock->listener) {
        sync::irq_state irq = sync::spin_lock_irqsave(sock->listener->lock);
        uint32_t mask = sock->listener->accept_queue.empty() ? 0 : sync::POLL_IN;
        sync::spin_unlock_irqrestore(sock->listener->lock, irq);

        return mask;
    }

    return 0;
}

static const resource::socket_ops g_unix_socket_ops = {
    .stream = true,
    .bind = unix_bind,
    .listen = unix_listen,
    .accept = unix_accept,
    .connect = unix_connect,
    .sendto = unix_sendto,
    .recvfrom = unix_recvfrom,
    .shutdown = unix_shutdown,
    .receive_lock = unix_receive_lock,
    .sendmsg = unix_sendmsg,
    .recvmsg = unix_recvmsg,
};

static const resource::socket_ops g_unix_seqpacket_ops = {
    .max_message = SEQPACKET_MAX_MESSAGE,
    .bind = unix_bind,
    .listen = unix_listen,
    .accept = unix_accept,
    .connect = unix_connect,
    .sendto = unix_sendto,
    .recvfrom = unix_recvfrom,
    .shutdown = unix_shutdown,
    .sendmsg = unix_sendmsg,
    .recvmsg = unix_recvmsg,
};

static const resource::resource_ops g_socket_ops = {
    .read = socket_read,
    .write = socket_write,
    .close = socket_close,
    .poll = socket_poll,
    .visit_held = unix_visit_held,
    .socket = &g_unix_socket_ops,
};

static const resource::resource_ops g_seqpacket_ops = {
    .read = socket_read,
    .write = socket_write,
    .close = socket_close,
    .poll = socket_poll,
    .visit_held = unix_visit_held,
    .socket = &g_unix_seqpacket_ops,
};

const resource::resource_ops* get_socket_ops(unix_socket_type type) {
    return type == unix_socket_type::seqpacket ? &g_seqpacket_ops : &g_socket_ops;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_unbound_socket(
    resource::resource_object** out,
    unix_socket_type type
) {
    if (!out) {
        return resource::ERR_INVAL;
    }

    auto* sock = heap::kalloc_new<unix_socket>();
    if (!sock) {
        return resource::ERR_NOMEM;
    }

    sock->state = SOCK_STATE_UNBOUND;
    sock->type = type;
    sock->lock = sync::SPINLOCK_INIT;
    sock->receive_lock.init();
    sock->poll_wq.init();
    sock->is_side_a = false;

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        heap::kfree_delete(sock);
        return resource::ERR_NOMEM;
    }

    obj->type = resource::resource_type::SOCKET;
    obj->ops = get_socket_ops(type);
    obj->impl = sock;

    *out = obj;
    return resource::OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_socket_pair(
    resource::resource_object** out_a,
    resource::resource_object** out_b,
    unix_socket_type type
) {
    if (!out_a || !out_b) {
        return resource::ERR_INVAL;
    }

    auto chan = create_channel(type);
    if (!chan) {
        return resource::ERR_NOMEM;
    }

    auto* sock_a = heap::kalloc_new<unix_socket>();
    if (!sock_a) {
        return resource::ERR_NOMEM;
    }

    sock_a->state = SOCK_STATE_CONNECTED;
    sock_a->type = type;
    sock_a->lock = sync::SPINLOCK_INIT;
    sock_a->receive_lock.init();
    sock_a->poll_wq.init();
    sock_a->is_side_a = true;
    sock_a->channel = chan;

    auto* sock_b = heap::kalloc_new<unix_socket>();
    if (!sock_b) {
        heap::kfree_delete(sock_a);
        return resource::ERR_NOMEM;
    }

    sock_b->state = SOCK_STATE_CONNECTED;
    sock_b->type = type;
    sock_b->lock = sync::SPINLOCK_INIT;
    sock_b->receive_lock.init();
    sock_b->poll_wq.init();
    sock_b->is_side_a = false;
    sock_b->channel = static_cast<rc::strong_ref<unix_channel>&&>(chan);

    auto* obj_a = heap::kalloc_new<resource::resource_object>();
    if (!obj_a) {
        heap::kfree_delete(sock_b);
        heap::kfree_delete(sock_a);
        return resource::ERR_NOMEM;
    }

    obj_a->type = resource::resource_type::SOCKET;
    obj_a->ops = get_socket_ops(type);
    obj_a->impl = sock_a;

    auto* obj_b = heap::kalloc_new<resource::resource_object>();
    if (!obj_b) {
        heap::kfree_delete(obj_a);
        heap::kfree_delete(sock_b);
        heap::kfree_delete(sock_a);
        return resource::ERR_NOMEM;
    }

    obj_b->type = resource::resource_type::SOCKET;
    obj_b->ops = get_socket_ops(type);
    obj_b->impl = sock_b;

    attach_poll_queue(sock_a);
    attach_poll_queue(sock_b);

    *out_a = obj_a;
    *out_b = obj_b;
    return resource::OK;
}

} // namespace socket
