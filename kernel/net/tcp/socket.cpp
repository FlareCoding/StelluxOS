#include "net/tcp/socket.h"
#include "net/tcp/conn.h"
#include "net/tcp/info.h"
#include "net/tcp/output.h"
#include "net/net.h"
#include "net/inet.h"
#include "net/interface.h"
#include "net/route.h"
#include "resource/socket_ops.h"
#include "fs/fstypes.h"
#include "sync/spinlock.h"
#include "sync/poll.h"
#include "sync/wait_queue.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
#include "clock/clock.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "dynpriv/dynpriv.h"
#include "common/string.h"

namespace net {
namespace tcp {

constexpr uint64_t SECOND_NS = 1000000000ULL;

static tcp_socket* g_sockets[MAX_SOCKETS];
static sync::spinlock g_sockets_lock = sync::SPINLOCK_INIT;

// Caller holds g_sockets_lock
static bool socket_conflicts_locked(const tcp_socket* self, const endpoint& local) {
    for (size_t i = 0; i < MAX_SOCKETS; i++) {
        tcp_socket* other = g_sockets[i];
        if (other && other != self && other->bound && endpoints_conflict(other->local, local)) {
            return true;
        }
    }

    return false;
}

__PRIVILEGED_CODE tcp_socket* socket_open() {
    tcp_socket* sock = heap::ualloc_new<tcp_socket>();
    if (!sock) {
        return nullptr;
    }

    sock->lock = sync::SPINLOCK_INIT;

    sync::irq_lock_guard guard(g_sockets_lock);
    for (size_t i = 0; i < MAX_SOCKETS; i++) {
        if (!g_sockets[i]) {
            g_sockets[i] = sock;
            return sock;
        }
    }

    heap::ufree_delete(sock);
    return nullptr;
}

// Only the program that asked is held up, never the kernel reaping its handles
__PRIVILEGED_CODE static bool closing_task_may_linger() {
    sched::task* task = sched::current();
    return task && task->group && task->cleanup_stage.load_acquire() == sched::TASK_CLEANUP_STAGE_ACTIVE;
}

__PRIVILEGED_CODE static bool fin_acknowledged_locked(const tcp_conn* conn) {
    return conn->state != tcp_state::fin_wait_1 && conn->state != tcp_state::closing &&
           conn->state != tcp_state::last_ack;
}

__PRIVILEGED_CODE bool wait_fin_acknowledged(tcp_conn* conn, uint64_t timeout_ns) {
    sched::task* task = sched::current();
    if (!task) {
        return false;
    }

    uint64_t deadline_ns = clock::now_ns() + timeout_ns;
    sync::poll_table pt;
    pt.init(task);
    sync::poll_subscribe(pt, conn->conn_wq);

    bool acknowledged = false;
    while (true) {
        {
            sync::irq_lock_guard guard(conn->lock);
            acknowledged = fin_acknowledged_locked(conn);
        }

        uint64_t now = clock::now_ns();
        if (acknowledged || pt.error.load_acquire() || now >= deadline_ns || signals::interrupt_pending(task)) {
            break;
        }

        sync::poll_wait(pt, deadline_ns - now);
    }

    sync::poll_cleanup(pt);

    return acknowledged;
}

__PRIVILEGED_CODE void socket_close(tcp_socket* sock) {
    {
        sync::irq_lock_guard guard(g_sockets_lock);
        for (size_t i = 0; i < MAX_SOCKETS; i++) {
            if (g_sockets[i] == sock) {
                g_sockets[i] = nullptr;
                break;
            }
        }
    }

    if (sock->listener) {
        listener_close(sock->listener.ptr());
    }

    if (sock->conn && sock->linger && sock->linger_seconds == 0) {
        abort_connection(sock->conn.ptr());
    } else if (sock->conn) {
        close_connection(sock->conn.ptr());
        if (sock->linger && closing_task_may_linger()) {
            (void)wait_fin_acknowledged(sock->conn.ptr(), sock->linger_seconds * SECOND_NS);
        }
    }

    heap::ufree_delete(sock);
}

__PRIVILEGED_CODE int32_t socket_listen(tcp_socket* sock, uint16_t backlog) {
    if (!sock->bound) {
        int32_t rc = socket_bind(sock, ipv4::UNSPECIFIED_ADDR, 0);
        if (rc != OK) {
            return rc;
        }
    }

    sync::irq_lock_guard guard(sock->lock);
    if (sock->conn || sock->connecting) {
        return ERR_INVALID;
    }

    if (sock->listener) {
        sync::irq_lock_guard listener_guard(sock->listener->lock);
        sock->listener->backlog = backlog;
        return OK;
    }

    tcp_listener* listener = alloc_listener(sock->local);
    if (!listener) {
        return ERR_NO_MEMORY;
    }

    listener->options = sock->options;
    listener->backlog = backlog;
    int32_t rc = listener_insert(listener);
    if (rc != OK) {
        if (listener->release()) {
            tcp_listener::ref_destroy(listener);
        }

        return rc;
    }

    sock->listener = rc::strong_ref<tcp_listener>::adopt(listener);
    return OK;
}

__PRIVILEGED_CODE int32_t socket_bind(tcp_socket* sock, const ipv4::ipv4_addr& addr, uint16_t port) {
    if (!addr.is_unspecified() && !find_interface_by_address(addr)) {
        return ERR_NOT_LOCAL;
    }

    // The allocator consults the socket table itself, so it runs before the lock,
    // and the port it picked is checked under the lock like an explicit one
    for (size_t attempt = 0; attempt < EPHEMERAL_BIND_ATTEMPTS; attempt++) {
        endpoint local = {addr, port, nullptr, false};
        if (port == 0) {
            int32_t rc = take_ephemeral_port(&local.port);
            if (rc != OK) {
                return rc;
            }
        }

        sync::irq_lock_guard sockets_guard(g_sockets_lock);
        sync::irq_lock_guard guard(sock->lock);
        if (sock->bound || sock->connecting) {
            return ERR_INVALID;
        }

        local.iface = sock->local.iface;
        local.reuseaddr = sock->local.reuseaddr;
        bool conflicts = socket_conflicts_locked(sock, local) || listener_conflicts(local);
        if (!conflicts) {
            sock->local = local;
            sock->bound = true;
            return OK;
        }

        if (port != 0) {
            return ERR_IN_USE;
        }
    }

    return ERR_FULL;
}

bool is_socket_port(uint16_t port) {
    bool taken = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_sockets_lock);
        for (size_t i = 0; i < MAX_SOCKETS; i++) {
            if (g_sockets[i] && g_sockets[i]->bound && g_sockets[i]->local.port == port) {
                taken = true;
                break;
            }
        }
    });

    return taken;
}

__PRIVILEGED_CODE static int32_t socket_bind(resource::resource_object* obj, const void* kaddr,
                                             size_t addrlen) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);

    ipv4::ipv4_addr addr;
    uint16_t port = 0;
    if (inet::parse_sockaddr(kaddr, addrlen, &addr, &port) != OK) {
        return resource::ERR_INVAL;
    }

    int32_t rc = socket_bind(sock, addr, port);
    return inet::map_net_error(rc == ERR_FULL ? ERR_IN_USE : rc);
}

__PRIVILEGED_CODE static int32_t socket_listen(resource::resource_object* obj, int32_t backlog) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);

    uint16_t depth = backlog < 1 ? 1 : (backlog > MAX_BACKLOG ? MAX_BACKLOG : static_cast<uint16_t>(backlog));
    int32_t rc = socket_listen(sock, depth);

    return inet::map_net_error(rc == ERR_FULL ? ERR_IN_USE : rc);
}

__PRIVILEGED_CODE static int32_t socket_accept(resource::resource_object* obj, resource::resource_object** new_obj,
                                               void* kaddr, size_t* addrlen, bool nonblock) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sched::task* task = sched::current();

    rc::strong_ref<tcp_listener> listener;
    {
        sync::irq_lock_guard guard(sock->lock);
        listener = sock->listener;
    }

    if (!listener) {
        return resource::ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(listener->lock);
    while (listener->accept_queue.empty() && !listener->closed && !nonblock &&
           !signals::interrupt_pending(task)) {
        irq = sync::wait(listener->accept_wq, listener->lock, irq);
    }

    tcp_conn* popped = listener->accept_queue.pop_front();
    if (popped) {
        listener->accept_count--;
    }

    bool closed = listener->closed;
    sync::spin_unlock_irqrestore(listener->lock, irq);

    if (!popped) {
        return closed ? resource::ERR_INVAL : (nonblock ? resource::ERR_AGAIN : resource::ERR_INTR);
    }

    rc::strong_ref<tcp_conn> conn = rc::strong_ref<tcp_conn>::adopt(popped);
    tcp_socket* child = socket_open();
    auto* child_obj = child ? heap::kalloc_new<resource::resource_object>() : nullptr;
    if (!child_obj) {
        abort_connection(conn.ptr());
        if (child) {
            socket_close(child);
        }

        return resource::ERR_NOMEM;
    }

    child->local = {conn->key.local_addr, conn->key.local_port, sock->local.iface, false};
    child->bound = true;
    child->linger = sock->linger;
    child->linger_seconds = sock->linger_seconds;
    child_obj->type = resource::resource_type::SOCKET;
    child_obj->ops = socket_ops();
    child_obj->impl = child;

    {
        sync::irq_lock_guard guard(conn->lock);
        child->options = {conn->nodelay, conn->snd_mss_cap};
        conn->owner = child_obj;
    }

    if (kaddr && addrlen &&
        inet::fill_sockaddr(kaddr, addrlen, conn->key.remote_addr, conn->key.remote_port) != OK) {
        *addrlen = 0;
    }

    child->conn = conn;
    *new_obj = child_obj;

    return resource::OK;
}

__PRIVILEGED_CODE static int32_t socket_getname(resource::resource_object* obj, void* kaddr,
                                                size_t* addrlen, bool peer) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sync::irq_lock_guard guard(sock->lock);

    if (peer) {
        if (!sock->conn) {
            return resource::ERR_NOTCONN;
        }

        {
            sync::irq_lock_guard conn_guard(sock->conn->lock);
            if (sock->conn->state == tcp_state::closed) {
                return resource::ERR_NOTCONN;
            }
        }

        const tuple& key = sock->conn->key;
        return inet::fill_sockaddr(kaddr, addrlen, key.remote_addr, key.remote_port) == OK
                   ? resource::OK : resource::ERR_INVAL;
    }

    if (inet::fill_sockaddr(kaddr, addrlen, sock->local.addr, sock->local.port) != OK) {
        return resource::ERR_INVAL;
    }

    return resource::OK;
}

// Caller holds the socket lock. What a connect finds already there decides
// whether it may begin: EISCONN and EALREADY as POSIX names them, and a
// finished attempt reports its error once and makes room for the next.
static int32_t connect_precondition_locked(tcp_socket* sock) {
    if (sock->listener) {
        return resource::ERR_ISCONN;
    }

    if (sock->connecting) {
        return resource::ERR_ALREADY;
    }

    if (!sock->conn) {
        return resource::OK;
    }

    tcp_state state;
    int32_t error;
    {
        sync::irq_lock_guard guard(sock->conn->lock);
        state = sock->conn->state;
        error = sock->conn->pending_error;
        sock->conn->pending_error = resource::OK;
    }

    if (state == tcp_state::syn_sent || state == tcp_state::syn_rcvd) {
        return resource::ERR_ALREADY;
    }

    if (state != tcp_state::closed) {
        return resource::ERR_ISCONN;
    }

    sock->conn.reset();
    return error != resource::OK ? error : resource::ERR_CONNREFUSED;
}

// Routes the destination, fills in the local half of `key` the socket left
// open, and opens the connection. Resource codes, since the socket reports them.
__PRIVILEGED_CODE static int32_t begin_active_open(interface* pinned, tuple* key, const conn_options& options,
                                                   rc::strong_ref<tcp_conn>* out) {
    route::route_result route;
    int32_t rc = pinned ? route::lookup_on(pinned, key->remote_addr, &route)
                        : route::lookup(key->remote_addr, &route);
    if (rc != OK) {
        return resource::ERR_NETUNREACH;
    }

    if (key->local_addr.is_unspecified()) {
        key->local_addr = route.source;
    }

    if (key->local_port == 0 && take_ephemeral_port(&key->local_port) != OK) {
        return resource::ERR_ADDRNOTAVAIL;
    }

    rc = open_active(*key, route.iface, out, options);
    if (rc == ERR_FULL) {
        return resource::ERR_NOBUFS;
    }

    return inet::map_net_error(rc);
}

__PRIVILEGED_CODE static int32_t socket_connect(resource::resource_object* obj, const void* kaddr,
                                                size_t addrlen, bool nonblock) {
    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sched::task* task = sched::current();

    ipv4::ipv4_addr dest;
    uint16_t port = 0;
    if (inet::parse_sockaddr(kaddr, addrlen, &dest, &port) != OK || port == 0 || dest.is_unspecified()) {
        return resource::ERR_INVAL;
    }

    interface* pinned = nullptr;
    conn_options options = {};
    tuple key = {ipv4::UNSPECIFIED_ADDR, dest, 0, port};
    {
        sync::irq_lock_guard guard(sock->lock);
        int32_t rc = connect_precondition_locked(sock);
        if (rc != resource::OK) {
            return rc;
        }

        // Held across the route and the open, so no second connect or listen slips in
        sock->connecting = true;
        pinned = sock->local.iface;
        options = sock->options;
        if (sock->bound) {
            key.local_addr = sock->local.addr;
            key.local_port = sock->local.port;
        }
    }

    rc::strong_ref<tcp_conn> conn;
    int32_t rc = begin_active_open(pinned, &key, options, &conn);

    {
        sync::irq_lock_guard guard(sock->lock);
        sock->connecting = false;
        if (rc == resource::OK) {
            sock->local = {key.local_addr, key.local_port, pinned, sock->local.reuseaddr};
            sock->bound = true;
            sock->conn = conn;
        }
    }

    if (rc != resource::OK) {
        return rc;
    }

    {
        sync::irq_lock_guard guard(conn->lock);
        conn->owner = obj;
    }

    if (nonblock) {
        return resource::ERR_INPROGRESS;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(conn->lock);
    while ((conn->state == tcp_state::syn_sent || conn->state == tcp_state::syn_rcvd) &&
           !signals::interrupt_pending(task)) {
        irq = sync::wait(conn->conn_wq, conn->lock, irq);
    }

    tcp_state state = conn->state;
    int32_t error = conn->pending_error;
    conn->pending_error = resource::OK;
    sync::spin_unlock_irqrestore(conn->lock, irq);

    if (state == tcp_state::established) {
        return resource::OK;
    }

    if (state != tcp_state::closed) {
        return resource::ERR_INTR;
    }

    // The failure is reported here and the socket is free for another attempt
    {
        sync::irq_lock_guard guard(sock->lock);
        if (sock->conn.ptr() == conn.ptr()) {
            sock->conn.reset();
        }
    }

    return error != resource::OK ? error : resource::ERR_CONNREFUSED;
}

// The size of an option's value, zero for one this socket does not have
static size_t option_size(int32_t level, int32_t optname) {
    if (level == inet::SOL_SOCKET) {
        switch (optname) {
        case inet::SO_REUSEADDR:
        case inet::SO_ERROR:
        case inet::SO_TYPE:
        case inet::SO_ACCEPTCONN:
            return sizeof(int32_t);
        case inet::SO_LINGER:
            return sizeof(inet::linger);
        default:
            return 0;
        }
    }

    if (level == inet::IPPROTO_TCP) {
        switch (optname) {
        case inet::TCP_NODELAY:
        case inet::TCP_MAXSEG:
        case inet::TCP_QUICKACK:
            return sizeof(int32_t);
        case inet::TCP_INFO:
            return sizeof(tcp_record);
        default:
            return 0;
        }
    }

    return 0;
}

__PRIVILEGED_CODE static int32_t take_pending_error(tcp_conn* conn) {
    sync::irq_lock_guard guard(conn->lock);
    int32_t error = conn->pending_error;
    conn->pending_error = resource::OK;

    return error;
}

// Caller holds the socket lock
__PRIVILEGED_CODE static int32_t option_value_locked(tcp_socket* sock, int32_t level, int32_t optname) {
    tcp_conn* conn = sock->conn.ptr();
    if (level == inet::SOL_SOCKET) {
        switch (optname) {
        case inet::SO_REUSEADDR:
            return sock->local.reuseaddr ? 1 : 0;
        case inet::SO_TYPE:
            return static_cast<int32_t>(inet::SOCK_STREAM);
        case inet::SO_ACCEPTCONN:
            return sock->listener ? 1 : 0;
        default:
            return conn ? take_pending_error(conn) : 0;
        }
    }

    switch (optname) {
    case inet::TCP_NODELAY:
        if (conn) {
            sync::irq_lock_guard guard(conn->lock);
            return conn->nodelay ? 1 : 0;
        }

        return sock->options.nodelay ? 1 : 0;
    case inet::TCP_MAXSEG:
        if (conn) {
            sync::irq_lock_guard guard(conn->lock);
            return conn->snd_mss;
        }

        return sock->options.snd_mss_cap != 0 ? sock->options.snd_mss_cap : DEFAULT_MSS;
    default:
        if (conn) {
            sync::irq_lock_guard guard(conn->lock);
            return conn->quick_acks > 0 ? 1 : 0;
        }

        return 0;
    }
}

__PRIVILEGED_CODE static int32_t socket_getsockopt(resource::resource_object* obj, int32_t level,
                                                   int32_t optname, void* optval, size_t* optlen) {
    size_t size = option_size(level, optname);
    if (size == 0) {
        return resource::ERR_NOPROTOOPT;
    }

    if (*optlen < size) {
        return resource::ERR_INVAL;
    }

    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    sync::irq_lock_guard guard(sock->lock);

    if (level == inet::IPPROTO_TCP && optname == inet::TCP_INFO) {
        tcp_record record = {};
        if (sock->conn) {
            describe_record(sock->conn.ptr(), &record);
        } else if (sock->listener) {
            describe_listener(sock->listener.ptr(), &record);
        } else {
            record.kind = INFO_KIND_CONNECTION;
        }

        string::memcpy(optval, &record, sizeof(record));
    } else if (level == inet::SOL_SOCKET && optname == inet::SO_LINGER) {
        inet::linger linger = {sock->linger ? 1 : 0, static_cast<int32_t>(sock->linger_seconds)};
        string::memcpy(optval, &linger, sizeof(linger));
    } else {
        int32_t value = option_value_locked(sock, level, optname);
        string::memcpy(optval, &value, sizeof(value));
    }

    *optlen = size;

    return resource::OK;
}

// Caller holds the socket lock
__PRIVILEGED_CODE static void set_mss_cap_locked(tcp_socket* sock, uint16_t cap) {
    sock->options.snd_mss_cap = cap;
    if (sock->listener) {
        sync::irq_lock_guard guard(sock->listener->lock);
        sock->listener->options.snd_mss_cap = cap;
    }

    if (sock->conn) {
        sync::irq_lock_guard guard(sock->conn->lock);
        sock->conn->snd_mss_cap = cap;
        if (sock->conn->rcv_mss != 0) {
            sock->conn->snd_mss = send_mss(sock->conn->iface, sock->conn->rcv_mss, cap);
        }
    }
}

// Caller holds the socket lock
__PRIVILEGED_CODE static void set_nodelay_locked(tcp_socket* sock, bool nodelay) {
    sock->options.nodelay = nodelay;
    if (sock->listener) {
        sync::irq_lock_guard guard(sock->listener->lock);
        sock->listener->options.nodelay = nodelay;
    }

    if (sock->conn) {
        sync::irq_lock_guard guard(sock->conn->lock);
        sock->conn->nodelay = nodelay;
    }
}

// Caller holds the connection lock
__PRIVILEGED_CODE static bool set_quickack_locked(tcp_conn* conn, bool on, segment_source* src) {
    conn->quick_acks = on ? MAX_QUICKACKS : 0;
    if (!on || !conn->ack_pending || !is_synchronized(conn->state)) {
        return false;
    }

    *src = snapshot_source(conn);
    mark_ack_sent_locked(conn);

    return true;
}

__PRIVILEGED_CODE static int32_t socket_setsockopt(resource::resource_object* obj, int32_t level,
                                                   int32_t optname, const void* optval, size_t optlen) {
    bool settable = (level == inet::SOL_SOCKET && (optname == inet::SO_REUSEADDR || optname == inet::SO_LINGER)) ||
                    (level == inet::IPPROTO_TCP && optname != inet::TCP_INFO);
    size_t size = option_size(level, optname);
    if (size == 0 || !settable) {
        return resource::ERR_NOPROTOOPT;
    }

    if (optlen < size) {
        return resource::ERR_INVAL;
    }

    int32_t value = 0;
    inet::linger linger = {};
    if (level == inet::SOL_SOCKET && optname == inet::SO_LINGER) {
        string::memcpy(&linger, optval, sizeof(linger));
    } else {
        string::memcpy(&value, optval, sizeof(value));
    }

    if (level == inet::IPPROTO_TCP && optname == inet::TCP_MAXSEG && value != 0 && (value < MIN_MSS || value > 0xFFFF)) {
        return resource::ERR_INVAL;
    }

    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    rc::strong_ref<tcp_conn> conn;
    bool send_ack = false;
    bool push = false;
    segment_source src = {};
    {
        sync::irq_lock_guard guard(sock->lock);
        conn = sock->conn;

        if (level == inet::SOL_SOCKET && optname == inet::SO_LINGER) {
            sock->linger = linger.on != 0;
            sock->linger_seconds = linger.seconds < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(linger.seconds);
        } else if (level == inet::SOL_SOCKET) {
            sock->local.reuseaddr = value != 0;
        } else if (optname == inet::TCP_NODELAY) {
            set_nodelay_locked(sock, value != 0);
            push = value != 0 && conn;
        } else if (optname == inet::TCP_MAXSEG) {
            set_mss_cap_locked(sock, static_cast<uint16_t>(value));
        } else if (conn) {
            sync::irq_lock_guard conn_guard(conn->lock);
            send_ack = set_quickack_locked(conn.ptr(), value != 0, &src);
        }
    }

    if (send_ack) {
        (void)send_control(src, FLAG_ACK);
    }

    if (push) {
        (void)output(conn.ptr());
    }

    return resource::OK;
}

__PRIVILEGED_CODE static rc::strong_ref<tcp_conn> connection_of(tcp_socket* sock) {
    sync::irq_lock_guard guard(sock->lock);
    return sock->conn;
}

// Caller holds the connection lock. RFC 1122 4.2.3.3: after a read frees
// room, the peer is told on its own only when the window opens from zero or,
// while the last advertised window was at most half the buffer and has at
// least doubled by no less than one segment.
__PRIVILEGED_CODE static bool window_update_owed_locked(tcp_conn* conn) {
    uint32_t advertised = conn->rcv_adv - conn->rcv_acked;
    update_receive_window_locked(conn);
    uint32_t growth = conn->rcv_nxt + conn->rcv_wnd - conn->rcv_adv;
    if (growth == 0 || !is_synchronized(conn->state)) {
        return false;
    }

    if (advertised == 0) {
        return true;
    }

    uint32_t half_buffer = static_cast<uint32_t>(conn->rcv_queue.limit() * CHUNK_PAYLOAD / 2);
    uint32_t threshold = half_buffer < conn->rcv_mss ? half_buffer : conn->rcv_mss;

    return advertised <= half_buffer && growth >= threshold && conn->rcv_wnd >= 2 * advertised;
}

__PRIVILEGED_CODE static bool reads_are_over_locked(const tcp_conn* conn) {
    return conn->state == tcp_state::closed || conn->fin_rcvd || conn->rcv_shutdown;
}

__PRIVILEGED_CODE static ssize_t receive(tcp_conn* conn, void* kdst, size_t count, uint32_t msg_flags) {
    sched::task* task = sched::current();
    if (!task) {
        return resource::ERR_IO;
    }

    bool nonblock = (msg_flags & inet::MSG_DONTWAIT) != 0;
    bool peek = (msg_flags & inet::MSG_PEEK) != 0;
    bool discard = (msg_flags & inet::MSG_TRUNC) != 0;
    bool whole = (msg_flags & inet::MSG_WAITALL) != 0;
    uint8_t* dst = static_cast<uint8_t*>(kdst);
    size_t copied = 0;

    sync::irq_state irq = sync::spin_lock_irqsave(conn->lock);
    for (;;) {
        size_t taken;
        if (discard) {
            size_t queued = conn->rcv_queue.size();
            taken = queued < count - copied ? queued : count - copied;
        } else {
            taken = conn->rcv_queue.copy_out(peek ? copied : 0, dst + copied, count - copied);
        }

        if (!peek) {
            (void)conn->rcv_queue.consume(taken);
        }

        copied += taken;
        if (copied == count || (copied > 0 && !whole) || reads_are_over_locked(conn) ||
            nonblock || signals::interrupt_pending(task)) {
            break;
        }

        // A peer at a closed window sends nothing until the room just freed is announced
        if (taken > 0 && !peek && window_update_owed_locked(conn)) {
            segment_source src = snapshot_source(conn);
            mark_ack_sent_locked(conn);

            sync::spin_unlock_irqrestore(conn->lock, irq);
            (void)send_control(src, FLAG_ACK);

            irq = sync::spin_lock_irqsave(conn->lock);
            continue;
        }

        irq = sync::wait(conn->rx_wq, conn->lock, irq);
    }

    ssize_t result;
    if (copied > 0) {
        result = static_cast<ssize_t>(copied);
    } else if (conn->state == tcp_state::closed && conn->pending_error != resource::OK) {
        result = conn->pending_error;
        conn->pending_error = resource::OK;
    } else if (reads_are_over_locked(conn)) {
        result = 0;
    } else {
        result = nonblock ? resource::ERR_AGAIN : resource::ERR_INTR;
    }

    bool update_window = copied > 0 && !peek && window_update_owed_locked(conn);
    segment_source src = {};
    if (update_window) {
        src = snapshot_source(conn);
        mark_ack_sent_locked(conn);
    }

    sync::spin_unlock_irqrestore(conn->lock, irq);

    if (update_window) {
        (void)send_control(src, FLAG_ACK);
    }

    return result;
}

__PRIVILEGED_CODE static int32_t write_refusal_locked(tcp_conn* conn) {
    if (conn->state == tcp_state::closed) {
        int32_t error = conn->pending_error;
        conn->pending_error = resource::OK;
        return error != resource::OK ? error : resource::ERR_PIPE;
    }

    if (conn->fin_pending || conn->fin_sent) {
        return resource::ERR_PIPE;
    }

    return resource::OK;
}

__PRIVILEGED_CODE static ssize_t transmit(tcp_conn* conn, const void* ksrc, size_t count, uint32_t msg_flags) {
    sched::task* task = sched::current();
    if (!task) {
        return resource::ERR_IO;
    }

    bool nonblock = (msg_flags & inet::MSG_DONTWAIT) != 0;
    const uint8_t* bytes = static_cast<const uint8_t*>(ksrc);
    size_t queued = 0;
    size_t pushed = 0;
    int32_t refusal = resource::OK;

    sync::irq_state irq = sync::spin_lock_irqsave(conn->lock);
    while (queued < count) {
        refusal = write_refusal_locked(conn);
        if (refusal != resource::OK) {
            break;
        }

        if (is_synchronized(conn->state)) {
            queued += conn->snd_queue.append(bytes + queued, count - queued);
            if (queued == count) {
                break;
            }
        }

        if (nonblock) {
            refusal = resource::ERR_AGAIN;
            break;
        }

        if (signals::interrupt_pending(task)) {
            refusal = resource::ERR_INTR;
            break;
        }

        // Room comes only from acknowledgments, so what is queued goes out before the wait
        if (pushed < queued) {
            sync::spin_unlock_irqrestore(conn->lock, irq);

            (void)output(conn);
            pushed = queued;

            irq = sync::spin_lock_irqsave(conn->lock);
            continue;
        }

        irq = sync::wait(conn->tx_wq, conn->lock, irq);
    }

    sync::spin_unlock_irqrestore(conn->lock, irq);

    if (queued > pushed) {
        (void)output(conn);
    }

    if (queued > 0) {
        return static_cast<ssize_t>(queued);
    }

    if (refusal == resource::ERR_PIPE && !(msg_flags & inet::MSG_NOSIGNAL)) {
        (void)signals::send_to_task(task, signals::SIGPIPE);
    }

    return refusal;
}

__PRIVILEGED_CODE static ssize_t socket_read(resource::resource_object* obj, void* kdst, size_t count, uint32_t flags) {
    rc::strong_ref<tcp_conn> conn = connection_of(static_cast<tcp_socket*>(obj->impl));
    if (!conn) {
        return resource::ERR_NOTCONN;
    }

    return receive(conn.ptr(), kdst, count, (flags & fs::O_NONBLOCK) ? inet::MSG_DONTWAIT : 0);
}

__PRIVILEGED_CODE static ssize_t socket_write(resource::resource_object* obj, const void* ksrc, size_t count, uint32_t flags) {
    rc::strong_ref<tcp_conn> conn = connection_of(static_cast<tcp_socket*>(obj->impl));
    if (!conn) {
        return resource::ERR_NOTCONN;
    }

    return transmit(conn.ptr(), ksrc, count, (flags & fs::O_NONBLOCK) ? inet::MSG_DONTWAIT : 0);
}

__PRIVILEGED_CODE static ssize_t socket_recvfrom(resource::resource_object* obj, void* kdst, size_t count,
                                                 uint32_t flags, void* kaddr, size_t* addrlen) {
    if (flags & inet::MSG_OOB) {
        return resource::ERR_UNSUP;
    }

    rc::strong_ref<tcp_conn> conn = connection_of(static_cast<tcp_socket*>(obj->impl));
    if (!conn) {
        return resource::ERR_NOTCONN;
    }

    ssize_t result = receive(conn.ptr(), kdst, count, flags);
    if (result >= 0 && kaddr && addrlen &&
        inet::fill_sockaddr(kaddr, addrlen, conn->key.remote_addr, conn->key.remote_port) != OK) {
        *addrlen = 0;
    }

    return result;
}

__PRIVILEGED_CODE static ssize_t socket_sendto(resource::resource_object* obj, const void* ksrc, size_t count,
                                               uint32_t flags, const void* kaddr, size_t addrlen) {
    if (flags & inet::MSG_OOB) {
        return resource::ERR_UNSUP;
    }

    rc::strong_ref<tcp_conn> conn = connection_of(static_cast<tcp_socket*>(obj->impl));
    if (!conn) {
        return resource::ERR_NOTCONN;
    }

    if (addrlen > 0) {
        ipv4::ipv4_addr dest;
        uint16_t port = 0;
        if (inet::parse_sockaddr(kaddr, addrlen, &dest, &port) != OK) {
            return resource::ERR_INVAL;
        }

        if (!(dest == conn->key.remote_addr) || port != conn->key.remote_port) {
            return resource::ERR_ISCONN;
        }
    }

    return transmit(conn.ptr(), ksrc, count, flags);
}

__PRIVILEGED_CODE static uint32_t socket_poll(resource::resource_object* obj, sync::poll_table* pt) {
    if (!obj || !obj->impl) {
        return sync::POLL_NVAL;
    }

    tcp_socket* sock = static_cast<tcp_socket*>(obj->impl);
    rc::strong_ref<tcp_listener> listener;
    rc::strong_ref<tcp_conn> conn;
    {
        sync::irq_lock_guard guard(sock->lock);
        listener = sock->listener;
        conn = sock->conn;
    }

    if (listener) {
        if (pt) {
            sync::poll_subscribe(*pt, listener->accept_wq);
        }

        sync::irq_lock_guard guard(listener->lock);
        return listener->accept_queue.empty() ? 0 : sync::POLL_IN;
    }

    if (conn) {
        if (pt) {
            sync::poll_subscribe(*pt, conn->conn_wq);
            sync::poll_subscribe(*pt, conn->rx_wq);
            sync::poll_subscribe(*pt, conn->tx_wq);
        }

        sync::irq_lock_guard guard(conn->lock);
        bool reads_over = reads_are_over_locked(conn.ptr());
        bool writes_over = conn->fin_pending || conn->fin_sent;
        size_t room_wanted = conn->snd_queue.limit() * CHUNK_PAYLOAD / 4;
        uint32_t events = conn->pending_error != resource::OK ? sync::POLL_ERR : 0;

        if (conn->rcv_queue.size() > 0 || reads_over) {
            events |= sync::POLL_IN;
        }

        if (reads_over) {
            events |= sync::POLL_RDHUP;
        }

        if (conn->state == tcp_state::closed || (reads_over && writes_over)) {
            events |= sync::POLL_HUP;
        }

        if (is_synchronized(conn->state) && !writes_over && conn->snd_queue.free_space() >= room_wanted) {
            events |= sync::POLL_OUT;
        }

        return events;
    }

    return sync::POLL_HUP;
}

__PRIVILEGED_CODE static int32_t socket_ioctl(resource::resource_object* obj, uint32_t cmd, uint64_t arg) {
    if (cmd != inet::FIONREAD) {
        return inet::socket_ioctl(obj, cmd, arg);
    }

    rc::strong_ref<tcp_conn> conn = connection_of(static_cast<tcp_socket*>(obj->impl));
    int32_t unread = 0;
    if (conn) {
        sync::irq_lock_guard guard(conn->lock);
        unread = static_cast<int32_t>(conn->rcv_queue.size());
    }

    int32_t rc = mm::uaccess::copy_to_user(reinterpret_cast<void*>(arg), &unread, sizeof(unread));

    return rc == mm::uaccess::OK ? resource::OK : resource::ERR_INVAL;
}

__PRIVILEGED_CODE static int32_t socket_shutdown(resource::resource_object* obj, int32_t how) {
    rc::strong_ref<tcp_conn> conn = connection_of(static_cast<tcp_socket*>(obj->impl));
    if (!conn) {
        return resource::ERR_NOTCONN;
    }

    {
        sync::irq_lock_guard guard(conn->lock);
        if (!is_synchronized(conn->state)) {
            return resource::ERR_NOTCONN;
        }
    }

    if (how == resource::SHUT_RD || how == resource::SHUT_RDWR) {
        shutdown_receive(conn.ptr());
    }

    if (how == resource::SHUT_WR || how == resource::SHUT_RDWR) {
        shutdown_send(conn.ptr());
    }

    return resource::OK;
}

__PRIVILEGED_CODE static void socket_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    socket_close(static_cast<tcp_socket*>(obj->impl));
    obj->impl = nullptr;
}

static const resource::socket_ops g_tcp_socket_ops = {
    .stream = true,
    .bind = socket_bind,
    .listen = socket_listen,
    .accept = socket_accept,
    .connect = socket_connect,
    .sendto = socket_sendto,
    .recvfrom = socket_recvfrom,
    .getname = socket_getname,
    .setsockopt = socket_setsockopt,
    .getsockopt = socket_getsockopt,
    .shutdown = socket_shutdown,
};

static const resource::resource_ops g_socket_ops = {
    .read = socket_read,
    .write = socket_write,
    .close = socket_close,
    .ioctl = socket_ioctl,
    .poll = socket_poll,
    .socket = &g_tcp_socket_ops,
};

const resource::resource_ops* socket_ops() {
    return &g_socket_ops;
}

} // namespace tcp
} // namespace net
