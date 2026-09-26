#include "socket/unix_socket.h"
#include "resource/socket_ops.h"
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

__PRIVILEGED_CODE void unix_channel::ref_destroy(unix_channel* self) {
    if (!self) {
        return;
    }

    ring_buffer_destroy(self->a_to_b.buf);
    ring_buffer_destroy(self->b_to_a.buf);
    heap::kfree_delete(self);
}

/**
 * A channel with a buffer for each direction, or null when memory runs out.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static rc::strong_ref<unix_channel> create_channel() {
    auto chan = rc::make_kref<unix_channel>();
    if (!chan) {
        return rc::strong_ref<unix_channel>();
    }

    chan->a_to_b.buf = ring_buffer_create(RING_BUFFER_DEFAULT_CAPACITY);
    if (!chan->a_to_b.buf) {
        return rc::strong_ref<unix_channel>();
    }

    chan->b_to_a.buf = ring_buffer_create(RING_BUFFER_DEFAULT_CAPACITY);
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
 * Writes every byte, waiting for room unless MSG_DONTWAIT, and reports a partial count on a later
 * error. A reader gone before any byte went out raises SIGPIPE unless MSG_NOSIGNAL.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t write_stream(unix_direction& dir, const uint8_t* bytes, size_t count,
                                              uint32_t msg_flags) {
    bool nonblock = (msg_flags & net::inet::MSG_DONTWAIT) != 0;
    size_t sent = 0;
    ssize_t n = 0;
    while (sent < count) {
        n = ring_buffer_write(dir.buf, bytes + sent, count - sent, nonblock);
        if (n < 0) {
            break;
        }

        sent += static_cast<size_t>(n);
    }

    if (sent > 0) {
        return static_cast<ssize_t>(sent);
    }

    if (n == RB_ERR_PIPE && !(msg_flags & net::inet::MSG_NOSIGNAL)) {
        (void)signals::send_to_task(sched::current(), signals::SIGPIPE);
    }

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
    return ring_buffer_read(inbound(sock).buf, static_cast<uint8_t*>(kdst), count, nonblock);
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
    return write_stream(outbound(sock), static_cast<const uint8_t*>(ksrc), count, msg_flags);
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
            sync::irq_state irq = sync::spin_lock_irqsave(sock->listener->lock);
            sock->listener->closed = true;
            while (pending_conn* pc = sock->listener->accept_queue.pop_front()) {
                sock->listener->pending_count--;
                resource::resource_release(pc->server_obj);
                heap::kfree(pc);
            }
            sync::spin_unlock_irqrestore(sock->listener->lock, irq);
            sync::wake_all(sock->listener->accept_wq);
        }
        // bound_node and listener strong_refs drop via ~unix_socket
        break;
    }

    case SOCK_STATE_CONNECTED: {
        if (sock->channel) {
            ring_buffer_close_write(outbound(sock).buf);
            ring_buffer_close_read(inbound(sock).buf);
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
    ls->accept_queue.init();
    ls->accept_wq.init();

    uint32_t bl = (backlog <= 0) ? DEFAULT_BACKLOG : static_cast<uint32_t>(backlog);
    if (bl > MAX_BACKLOG) {
        bl = MAX_BACKLOG;
    }
    ls->backlog = bl;
    ls->pending_count = 0;

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

    auto chan = create_channel();
    if (!chan) {
        return resource::ERR_NOMEM;
    }

    auto* server_sock = heap::kalloc_new<unix_socket>();
    if (!server_sock) {
        return resource::ERR_NOMEM;
    }

    server_sock->state = SOCK_STATE_CONNECTED;
    server_sock->lock = sync::SPINLOCK_INIT;
    server_sock->is_side_a = true;
    server_sock->channel = chan;

    auto* server_obj = heap::kalloc_new<resource::resource_object>();
    if (!server_obj) {
        heap::kfree_delete(server_sock);
        return resource::ERR_NOMEM;
    }

    server_obj->type = resource::resource_type::SOCKET;
    server_obj->ops = get_socket_ops();
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

    client_sock->channel = static_cast<rc::strong_ref<unix_channel>&&>(chan);
    client_sock->is_side_a = false;
    barrier::smp_write();
    client_sock->state = SOCK_STATE_CONNECTED;

    pc->server_obj = server_obj;
    ls_ref->accept_queue.push_back(pc);
    ls_ref->pending_count++;
    sync::spin_unlock_irqrestore(ls_ref->lock, irq);
    sync::wake_one(ls_ref->accept_wq);

    return resource::OK;
}

/**
 * A connected stream takes no destination, and out of band data is not supported.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t unix_sendto(
    resource::resource_object* obj, const void* ksrc, size_t count, uint32_t flags, const void*, size_t addrlen
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

    return write_stream(outbound(sock), static_cast<const uint8_t*>(ksrc), count, flags);
}

/**
 * A null destination discards the bytes instead of copying them, and a peek leaves them
 * queued. The peer of a pair has no address to report.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static ssize_t unix_recvfrom(
    resource::resource_object* obj, void* kdst, size_t count, uint32_t flags, void*, size_t* addrlen
) {
    if (!obj || !obj->impl) {
        return resource::ERR_INVAL;
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

    ring_buffer* rb = inbound(sock).buf;
    bool nonblock = (flags & net::inet::MSG_DONTWAIT) != 0;
    if (kdst && !(flags & net::inet::MSG_PEEK)) {
        return ring_buffer_read(rb, static_cast<uint8_t*>(kdst), count, nonblock);
    }

    ssize_t queued = ring_buffer_wait_readable(rb, nonblock);
    if (queued <= 0) {
        return queued;
    }

    if (!kdst) {
        return static_cast<ssize_t>(ring_buffer_skip(rb, count));
    }

    return static_cast<ssize_t>(ring_buffer_peek(rb, static_cast<uint8_t*>(kdst), count));
}

__PRIVILEGED_CODE static uint32_t socket_poll(
    resource::resource_object* obj, sync::poll_table* pt
) {
    if (!obj || !obj->impl) {
        return sync::POLL_NVAL;
    }

    auto* sock = static_cast<unix_socket*>(obj->impl);

    if (sock->state == SOCK_STATE_CONNECTED) {
        return ring_buffer_poll_read(inbound(sock).buf, pt)
             | ring_buffer_poll_write(outbound(sock).buf, pt);
    }

    if (sock->state == SOCK_STATE_LISTENING && sock->listener) {
        if (pt) {
            sync::poll_subscribe(*pt, sock->listener->accept_wq);
        }

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
};

static const resource::resource_ops g_socket_ops = {
    .read = socket_read,
    .write = socket_write,
    .close = socket_close,
    .poll = socket_poll,
    .socket = &g_unix_socket_ops,
};

const resource::resource_ops* get_socket_ops() {
    return &g_socket_ops;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_unbound_socket(
    resource::resource_object** out
) {
    if (!out) {
        return resource::ERR_INVAL;
    }

    auto* sock = heap::kalloc_new<unix_socket>();
    if (!sock) {
        return resource::ERR_NOMEM;
    }

    sock->state = SOCK_STATE_UNBOUND;
    sock->lock = sync::SPINLOCK_INIT;
    sock->is_side_a = false;

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        heap::kfree_delete(sock);
        return resource::ERR_NOMEM;
    }

    obj->type = resource::resource_type::SOCKET;
    obj->ops = &g_socket_ops;
    obj->impl = sock;

    *out = obj;
    return resource::OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_socket_pair(
    resource::resource_object** out_a,
    resource::resource_object** out_b
) {
    if (!out_a || !out_b) {
        return resource::ERR_INVAL;
    }

    auto chan = create_channel();
    if (!chan) {
        return resource::ERR_NOMEM;
    }

    auto* sock_a = heap::kalloc_new<unix_socket>();
    if (!sock_a) {
        return resource::ERR_NOMEM;
    }

    sock_a->state = SOCK_STATE_CONNECTED;
    sock_a->lock = sync::SPINLOCK_INIT;
    sock_a->is_side_a = true;
    sock_a->channel = chan;

    auto* sock_b = heap::kalloc_new<unix_socket>();
    if (!sock_b) {
        heap::kfree_delete(sock_a);
        return resource::ERR_NOMEM;
    }

    sock_b->state = SOCK_STATE_CONNECTED;
    sock_b->lock = sync::SPINLOCK_INIT;
    sock_b->is_side_a = false;
    sock_b->channel = static_cast<rc::strong_ref<unix_channel>&&>(chan);

    auto* obj_a = heap::kalloc_new<resource::resource_object>();
    if (!obj_a) {
        heap::kfree_delete(sock_b);
        heap::kfree_delete(sock_a);
        return resource::ERR_NOMEM;
    }

    obj_a->type = resource::resource_type::SOCKET;
    obj_a->ops = &g_socket_ops;
    obj_a->impl = sock_a;

    auto* obj_b = heap::kalloc_new<resource::resource_object>();
    if (!obj_b) {
        heap::kfree_delete(obj_a);
        heap::kfree_delete(sock_b);
        heap::kfree_delete(sock_a);
        return resource::ERR_NOMEM;
    }

    obj_b->type = resource::resource_type::SOCKET;
    obj_b->ops = &g_socket_ops;
    obj_b->impl = sock_b;

    *out_a = obj_a;
    *out_b = obj_b;
    return resource::OK;
}

} // namespace socket
