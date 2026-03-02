#include "net/unix_stream.h"

#include "common/hash.h"
#include "common/hashmap.h"
#include "common/list.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"
#include "rc/ref_counted.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace net::unix_stream {

constexpr uint32_t STREAM_BUFFER_SIZE = 4096;
constexpr uint32_t REGISTRY_BUCKET_COUNT = 256;
constexpr uint32_t LISTEN_BACKLOG_MIN = 1;
constexpr uint32_t LISTEN_BACKLOG_MAX = 64;

enum class socket_state : uint8_t {
    CREATED = 0,
    BOUND = 1,
    LISTENING = 2,
    CONNECTED = 3,
    CLOSED = 4,
};

struct ring_buffer {
    uint32_t head;
    uint32_t used;
    uint8_t bytes[STREAM_BUFFER_SIZE];

    void init() {
        head = 0;
        used = 0;
    }

    [[nodiscard]] uint32_t available() const {
        return used;
    }

    [[nodiscard]] uint32_t free_space() const {
        return STREAM_BUFFER_SIZE - used;
    }

    size_t write(const uint8_t* src, size_t count) {
        if (!src || count == 0 || used == STREAM_BUFFER_SIZE) {
            return 0;
        }

        size_t written = 0;
        while (written < count && used < STREAM_BUFFER_SIZE) {
            uint32_t tail = (head + used) % STREAM_BUFFER_SIZE;
            size_t span = STREAM_BUFFER_SIZE - tail;
            size_t want = count - written;
            size_t free = STREAM_BUFFER_SIZE - used;
            size_t chunk = span;
            if (chunk > want) {
                chunk = want;
            }
            if (chunk > free) {
                chunk = free;
            }

            string::memcpy(bytes + tail, src + written, chunk);
            used += static_cast<uint32_t>(chunk);
            written += chunk;
        }

        return written;
    }

    size_t read(uint8_t* dst, size_t count) {
        if (!dst || count == 0 || used == 0) {
            return 0;
        }

        size_t consumed = 0;
        while (consumed < count && used > 0) {
            size_t span = STREAM_BUFFER_SIZE - head;
            size_t want = count - consumed;
            size_t have = used;
            size_t chunk = span;
            if (chunk > want) {
                chunk = want;
            }
            if (chunk > have) {
                chunk = have;
            }

            string::memcpy(dst + consumed, bytes + head, chunk);
            head = (head + static_cast<uint32_t>(chunk)) % STREAM_BUFFER_SIZE;
            used -= static_cast<uint32_t>(chunk);
            consumed += chunk;
        }

        return consumed;
    }
};

class stream_connection : public rc::ref_counted<stream_connection> {
public:
    stream_connection() : lock(sync::SPINLOCK_INIT), side_open{true, true} {
        channel[0].init();
        channel[1].init();
        data_wq[0].init();
        data_wq[1].init();
        space_wq[0].init();
        space_wq[1].init();
    }

    static void ref_destroy(stream_connection* self) {
        if (!self) {
            return;
        }
        self->~stream_connection();
        heap::kfree(self);
    }

    sync::spinlock lock;
    ring_buffer channel[2]; // channel[0]=0->1, channel[1]=1->0
    sync::wait_queue data_wq[2];
    sync::wait_queue space_wq[2];
    bool side_open[2];
};

class stream_socket : public rc::ref_counted<stream_socket> {
public:
    explicit stream_socket(bool start_nonblocking)
        : lock(sync::SPINLOCK_INIT)
        , state(socket_state::CREATED)
        , nonblocking(start_nonblocking)
        , registered(false)
        , path{}
        , backlog_limit(0)
        , pending_count(0)
        , inflight_count(0)
        , pending_link{}
        , conn(nullptr)
        , conn_side(0)
        , registry_link{} {
        path.len = 0;
        pending.init();
        accept_wq.init();
        backlog_wq.init();
    }

    static void ref_destroy(stream_socket* self);

    sync::spinlock lock;
    socket_state state;
    bool nonblocking;
    bool registered;
    socket_path path;

    uint32_t backlog_limit;
    uint32_t pending_count;
    uint32_t inflight_count;
    list::node pending_link;
    list::head<stream_socket, &stream_socket::pending_link> pending;
    sync::wait_queue accept_wq;
    sync::wait_queue backlog_wq;

    stream_connection* conn;
    uint8_t conn_side;

    hashmap::node registry_link;
};

struct registry_key_ops {
    using key_type = const socket_path*;

    static key_type key_of(const stream_socket& entry) {
        return &entry.path;
    }

    static uint64_t hash(const key_type& key) {
        if (!key) {
            return 0;
        }
        uint64_t h = hash::bytes(key->bytes, key->len);
        return hash::combine(h, key->len);
    }

    static bool equal(const key_type& a, const key_type& b) {
        if (!a || !b) {
            return a == b;
        }
        if (a->len != b->len) {
            return false;
        }
        if (a->len == 0) {
            return true;
        }
        return string::memcmp(a->bytes, b->bytes, a->len) == 0;
    }
};

__PRIVILEGED_BSS static bool g_initialized;
__PRIVILEGED_BSS static sync::spinlock g_registry_lock;
__PRIVILEGED_BSS static hashmap::bucket g_registry_buckets[REGISTRY_BUCKET_COUNT];
__PRIVILEGED_BSS static hashmap::map<
    stream_socket,
    &stream_socket::registry_link,
    registry_key_ops
> g_registry;

static void connection_release(stream_connection* connection) {
    if (!connection) {
        return;
    }
    if (connection->release()) {
        stream_connection::ref_destroy(connection);
    }
}

static void connection_add_ref(stream_connection* connection) {
    if (!connection) {
        return;
    }
    connection->add_ref();
}

static void socket_set_path(stream_socket* socket, const socket_path& path) {
    socket->path.len = path.len;
    if (path.len > 0) {
        string::memcpy(socket->path.bytes, path.bytes, path.len);
    }
}

static void socket_clear_path(stream_socket* socket) {
    socket->path.len = 0;
}

static int32_t capture_connected(
    stream_socket* socket,
    stream_connection** out_connection,
    uint8_t* out_side,
    bool* out_nonblocking
) {
    sync::irq_state irq = sync::spin_lock_irqsave(socket->lock);
    if (socket->state == socket_state::CLOSED) {
        sync::spin_unlock_irqrestore(socket->lock, irq);
        return ERR_BADF;
    }
    if (socket->state != socket_state::CONNECTED || !socket->conn) {
        sync::spin_unlock_irqrestore(socket->lock, irq);
        return ERR_NOTCONN;
    }

    connection_add_ref(socket->conn);
    *out_connection = socket->conn;
    *out_side = socket->conn_side;
    *out_nonblocking = socket->nonblocking;
    sync::spin_unlock_irqrestore(socket->lock, irq);
    return OK;
}

static void wake_all_connection_waiters(stream_connection* connection) {
    sync::wake_all(connection->data_wq[0]);
    sync::wake_all(connection->data_wq[1]);
    sync::wake_all(connection->space_wq[0]);
    sync::wake_all(connection->space_wq[1]);
}

static void close_connected_side(stream_connection* connection, uint8_t side) {
    sync::irq_state irq = sync::spin_lock_irqsave(connection->lock);
    if (!connection->side_open[side]) {
        sync::spin_unlock_irqrestore(connection->lock, irq);
        return;
    }
    connection->side_open[side] = false;
    sync::spin_unlock_irqrestore(connection->lock, irq);
    wake_all_connection_waiters(connection);
}

static void unregister_listener(stream_socket* socket) {
    bool removed = false;

    sync::irq_state registry_irq = sync::spin_lock_irqsave(g_registry_lock);
    sync::irq_state socket_irq = sync::spin_lock_irqsave(socket->lock);
    if (socket->registered) {
        g_registry.remove(*socket);
        socket->registered = false;
        socket_clear_path(socket);
        removed = true;
    }
    sync::spin_unlock_irqrestore(socket->lock, socket_irq);
    sync::spin_unlock_irqrestore(g_registry_lock, registry_irq);

    if (removed) {
        release(socket); // Drop registry ownership reference.
    }
}

static int32_t reserve_listener_backlog(stream_socket* listener, bool connect_nonblocking) {
    sync::irq_state irq = sync::spin_lock_irqsave(listener->lock);
    if (listener->state != socket_state::LISTENING) {
        sync::spin_unlock_irqrestore(listener->lock, irq);
        return ERR_CONNREFUSED;
    }

    while ((listener->pending_count + listener->inflight_count) >= listener->backlog_limit) {
        if (connect_nonblocking) {
            sync::spin_unlock_irqrestore(listener->lock, irq);
            return ERR_AGAIN;
        }
        irq = sync::wait(listener->backlog_wq, listener->lock, irq);
        if (listener->state != socket_state::LISTENING) {
            sync::spin_unlock_irqrestore(listener->lock, irq);
            return ERR_CONNREFUSED;
        }
    }

    listener->inflight_count++;
    sync::spin_unlock_irqrestore(listener->lock, irq);
    return OK;
}

static void unreserve_listener_backlog(stream_socket* listener) {
    sync::irq_state irq = sync::spin_lock_irqsave(listener->lock);
    if (listener->inflight_count > 0) {
        listener->inflight_count--;
    }
    sync::spin_unlock_irqrestore(listener->lock, irq);
    sync::wake_one(listener->backlog_wq);
}

void stream_socket::ref_destroy(stream_socket* self) {
    if (!self) {
        return;
    }

    close(self);
    self->~stream_socket();
    heap::kfree(self);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    g_registry_lock = sync::SPINLOCK_INIT;
    g_registry.init(g_registry_buckets, REGISTRY_BUCKET_COUNT);
    g_initialized = true;
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t make_path(
    const char* path_bytes,
    size_t path_len,
    socket_path* out
) {
    if (!path_bytes || !out) {
        return ERR_INVAL;
    }
    if (path_len == 0 || path_len > SOCKET_PATH_MAX) {
        return ERR_INVAL;
    }
    if (path_bytes[0] == '\0') {
        return ERR_INVAL; // Abstract namespace is intentionally unsupported.
    }

    size_t effective_len = path_len;
    for (size_t i = 0; i < path_len; i++) {
        if (path_bytes[i] == '\0') {
            effective_len = i;
            break;
        }
    }

    if (effective_len == 0 || effective_len > SOCKET_PATH_MAX) {
        return ERR_INVAL;
    }

    out->len = static_cast<uint16_t>(effective_len);
    string::memcpy(out->bytes, path_bytes, effective_len);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t make_path_cstr(
    const char* cstr,
    socket_path* out
) {
    if (!cstr || !out) {
        return ERR_INVAL;
    }
    size_t len = string::strnlen(cstr, SOCKET_PATH_MAX + 1);
    if (len == 0 || len > SOCKET_PATH_MAX) {
        return ERR_INVAL;
    }
    return make_path(cstr, len, out);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_socket(bool nonblocking, stream_socket** out_socket) {
    if (!g_initialized || !out_socket) {
        return ERR_INVAL;
    }

    void* mem = heap::kzalloc(sizeof(stream_socket));
    if (!mem) {
        return ERR_NOMEM;
    }

    auto* socket = new (mem) stream_socket(nonblocking);
    *out_socket = socket;
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t bind(stream_socket* socket, const socket_path& path) {
    if (!g_initialized || !socket) {
        return ERR_INVAL;
    }
    if (path.len == 0 || path.len > SOCKET_PATH_MAX) {
        return ERR_INVAL;
    }

    sync::irq_state registry_irq = sync::spin_lock_irqsave(g_registry_lock);
    sync::irq_state socket_irq = sync::spin_lock_irqsave(socket->lock);

    if (socket->state != socket_state::CREATED || socket->registered) {
        sync::spin_unlock_irqrestore(socket->lock, socket_irq);
        sync::spin_unlock_irqrestore(g_registry_lock, registry_irq);
        return ERR_INVAL;
    }

    if (g_registry.find(&path) != nullptr) {
        sync::spin_unlock_irqrestore(socket->lock, socket_irq);
        sync::spin_unlock_irqrestore(g_registry_lock, registry_irq);
        return ERR_ADDRINUSE;
    }

    add_ref(socket); // Registry ownership reference.
    socket_set_path(socket, path);
    socket->registered = true;
    socket->state = socket_state::BOUND;
    g_registry.insert(socket);

    sync::spin_unlock_irqrestore(socket->lock, socket_irq);
    sync::spin_unlock_irqrestore(g_registry_lock, registry_irq);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t listen(stream_socket* socket, uint32_t backlog) {
    if (!socket) {
        return ERR_INVAL;
    }

    uint32_t clamped = backlog;
    if (clamped < LISTEN_BACKLOG_MIN) {
        clamped = LISTEN_BACKLOG_MIN;
    }
    if (clamped > LISTEN_BACKLOG_MAX) {
        clamped = LISTEN_BACKLOG_MAX;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(socket->lock);
    if (socket->state != socket_state::BOUND) {
        sync::spin_unlock_irqrestore(socket->lock, irq);
        return ERR_INVAL;
    }

    socket->backlog_limit = clamped;
    socket->state = socket_state::LISTENING;
    sync::spin_unlock_irqrestore(socket->lock, irq);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t connect(stream_socket* socket, const socket_path& path) {
    if (!socket) {
        return ERR_INVAL;
    }

    bool connect_nonblocking = false;
    sync::irq_state socket_irq = sync::spin_lock_irqsave(socket->lock);
    if (socket->state == socket_state::CLOSED) {
        sync::spin_unlock_irqrestore(socket->lock, socket_irq);
        return ERR_BADF;
    }
    if (socket->state != socket_state::CREATED) {
        sync::spin_unlock_irqrestore(socket->lock, socket_irq);
        return ERR_INVAL;
    }
    connect_nonblocking = socket->nonblocking;
    sync::spin_unlock_irqrestore(socket->lock, socket_irq);

    stream_socket* listener = nullptr;
    sync::irq_state registry_irq = sync::spin_lock_irqsave(g_registry_lock);
    listener = g_registry.find(&path);
    if (listener) {
        add_ref(listener);
    }
    sync::spin_unlock_irqrestore(g_registry_lock, registry_irq);
    if (!listener) {
        return ERR_CONNREFUSED;
    }

    int32_t reserve_rc = reserve_listener_backlog(listener, connect_nonblocking);
    if (reserve_rc != OK) {
        release(listener);
        return reserve_rc;
    }

    stream_connection* connection = nullptr;
    stream_socket* server_socket = nullptr;

    void* conn_mem = heap::kzalloc(sizeof(stream_connection));
    if (!conn_mem) {
        unreserve_listener_backlog(listener);
        release(listener);
        return ERR_NOMEM;
    }
    connection = new (conn_mem) stream_connection();

    int32_t create_rc = create_socket(false, &server_socket);
    if (create_rc != OK) {
        unreserve_listener_backlog(listener);
        connection_release(connection);
        release(listener);
        return create_rc;
    }

    sync::irq_state listener_irq = sync::spin_lock_irqsave(listener->lock);
    sync::irq_state client_irq = sync::spin_lock_irqsave(socket->lock);

    if (listener->state != socket_state::LISTENING || !listener->registered) {
        sync::spin_unlock_irqrestore(socket->lock, client_irq);
        if (listener->inflight_count > 0) {
            listener->inflight_count--;
        }
        sync::spin_unlock_irqrestore(listener->lock, listener_irq);
        sync::wake_one(listener->backlog_wq);
        connection_release(connection);
        close(server_socket);
        release(server_socket);
        release(listener);
        return ERR_CONNREFUSED;
    }

    if (socket->state != socket_state::CREATED) {
        sync::spin_unlock_irqrestore(socket->lock, client_irq);
        if (listener->inflight_count > 0) {
            listener->inflight_count--;
        }
        sync::spin_unlock_irqrestore(listener->lock, listener_irq);
        sync::wake_one(listener->backlog_wq);
        connection_release(connection);
        close(server_socket);
        release(server_socket);
        release(listener);
        return ERR_INVAL;
    }

    socket->conn = connection;
    socket->conn_side = 0;
    socket->state = socket_state::CONNECTED;

    connection_add_ref(connection); // Server endpoint ownership.
    server_socket->conn = connection;
    server_socket->conn_side = 1;
    server_socket->state = socket_state::CONNECTED;

    add_ref(server_socket); // Queue ownership.
    listener->pending.push_back(server_socket);
    listener->pending_count++;
    if (listener->inflight_count > 0) {
        listener->inflight_count--;
    }

    sync::spin_unlock_irqrestore(socket->lock, client_irq);
    sync::spin_unlock_irqrestore(listener->lock, listener_irq);

    sync::wake_one(listener->accept_wq);

    release(server_socket); // Drop creator ownership; queue keeps one.
    release(listener);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t accept(stream_socket* listener, stream_socket** out_socket) {
    if (!listener || !out_socket) {
        return ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(listener->lock);
    if (listener->state != socket_state::LISTENING) {
        sync::spin_unlock_irqrestore(listener->lock, irq);
        return ERR_INVAL;
    }

    while (listener->pending_count == 0) {
        if (listener->nonblocking) {
            sync::spin_unlock_irqrestore(listener->lock, irq);
            return ERR_AGAIN;
        }

        irq = sync::wait(listener->accept_wq, listener->lock, irq);
        if (listener->state != socket_state::LISTENING && listener->pending_count == 0) {
            sync::spin_unlock_irqrestore(listener->lock, irq);
            return ERR_BADF;
        }
    }

    stream_socket* accepted = listener->pending.pop_front();
    if (accepted && listener->pending_count > 0) {
        listener->pending_count--;
    } else if (!accepted) {
        sync::spin_unlock_irqrestore(listener->lock, irq);
        return ERR_INVAL;
    }

    sync::spin_unlock_irqrestore(listener->lock, irq);
    sync::wake_one(listener->backlog_wq);

    *out_socket = accepted; // Transfers queue ownership reference.
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t send(stream_socket* socket, const void* ksrc, size_t count) {
    if (!socket || !ksrc) {
        return ERR_INVAL;
    }
    if (count == 0) {
        return 0;
    }

    stream_connection* connection = nullptr;
    uint8_t side = 0;
    bool nonblocking = false;
    int32_t rc = capture_connected(socket, &connection, &side, &nonblocking);
    if (rc != OK) {
        return rc;
    }

    const uint8_t* src = static_cast<const uint8_t*>(ksrc);
    ssize_t result = ERR_INVAL;

    sync::irq_state irq = sync::spin_lock_irqsave(connection->lock);
    for (;;) {
        uint8_t peer = static_cast<uint8_t>(1 - side);

        if (!connection->side_open[peer]) {
            sync::spin_unlock_irqrestore(connection->lock, irq);
            result = ERR_PIPE;
            break;
        }

        ring_buffer& out = connection->channel[side];
        size_t wrote = out.write(src, count);
        if (wrote > 0) {
            sync::spin_unlock_irqrestore(connection->lock, irq);
            sync::wake_one(connection->data_wq[side]);
            result = static_cast<ssize_t>(wrote);
            break;
        }

        if (nonblocking) {
            sync::spin_unlock_irqrestore(connection->lock, irq);
            result = ERR_AGAIN;
            break;
        }

        irq = sync::wait(connection->space_wq[side], connection->lock, irq);
    }

    connection_release(connection);
    return result;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t recv(stream_socket* socket, void* kdst, size_t count) {
    if (!socket || !kdst) {
        return ERR_INVAL;
    }
    if (count == 0) {
        return 0;
    }

    stream_connection* connection = nullptr;
    uint8_t side = 0;
    bool nonblocking = false;
    int32_t rc = capture_connected(socket, &connection, &side, &nonblocking);
    if (rc != OK) {
        return rc;
    }

    uint8_t* dst = static_cast<uint8_t*>(kdst);
    ssize_t result = ERR_INVAL;
    uint8_t peer = static_cast<uint8_t>(1 - side);

    sync::irq_state irq = sync::spin_lock_irqsave(connection->lock);
    for (;;) {
        ring_buffer& incoming = connection->channel[peer];
        size_t read_count = incoming.read(dst, count);
        if (read_count > 0) {
            sync::spin_unlock_irqrestore(connection->lock, irq);
            sync::wake_one(connection->space_wq[peer]);
            result = static_cast<ssize_t>(read_count);
            break;
        }

        if (!connection->side_open[peer]) {
            sync::spin_unlock_irqrestore(connection->lock, irq);
            result = 0;
            break;
        }

        if (nonblocking) {
            sync::spin_unlock_irqrestore(connection->lock, irq);
            result = ERR_AGAIN;
            break;
        }

        irq = sync::wait(connection->data_wq[peer], connection->lock, irq);
    }

    connection_release(connection);
    return result;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t close(stream_socket* socket) {
    if (!socket) {
        return ERR_INVAL;
    }

    bool need_unregister = false;
    bool was_listener = false;
    stream_connection* connection = nullptr;
    uint8_t side = 0;
    list::head<stream_socket, &stream_socket::pending_link> pending_batch;
    pending_batch.init();

    sync::irq_state irq = sync::spin_lock_irqsave(socket->lock);
    if (socket->state == socket_state::CLOSED) {
        sync::spin_unlock_irqrestore(socket->lock, irq);
        return OK;
    }

    if (socket->registered) {
        need_unregister = true;
    }
    if (socket->state == socket_state::LISTENING) {
        was_listener = true;
        while (stream_socket* pending = socket->pending.pop_front()) {
            if (socket->pending_count > 0) {
                socket->pending_count--;
            }
            pending_batch.push_back(pending);
        }
        socket->inflight_count = 0;
    }
    if (socket->state == socket_state::CONNECTED && socket->conn) {
        connection = socket->conn; // Transfer this socket's owned ref to local.
        side = socket->conn_side;
        socket->conn = nullptr;
    }

    socket->state = socket_state::CLOSED;
    socket->backlog_limit = 0;
    sync::spin_unlock_irqrestore(socket->lock, irq);

    if (need_unregister) {
        unregister_listener(socket);
    }

    if (was_listener) {
        sync::wake_all(socket->accept_wq);
        sync::wake_all(socket->backlog_wq);

        while (stream_socket* pending = pending_batch.pop_front()) {
            close(pending);
            release(pending); // Drop listener queue ownership.
        }
    }

    if (connection) {
        close_connected_side(connection, side);
        connection_release(connection);
    }

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t set_nonblocking(stream_socket* socket, bool nonblocking) {
    if (!socket) {
        return ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(socket->lock);
    if (socket->state == socket_state::CLOSED) {
        sync::spin_unlock_irqrestore(socket->lock, irq);
        return ERR_BADF;
    }
    socket->nonblocking = nonblocking;
    sync::spin_unlock_irqrestore(socket->lock, irq);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t get_nonblocking(stream_socket* socket, bool* out_nonblocking) {
    if (!socket || !out_nonblocking) {
        return ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(socket->lock);
    if (socket->state == socket_state::CLOSED) {
        sync::spin_unlock_irqrestore(socket->lock, irq);
        return ERR_BADF;
    }
    *out_nonblocking = socket->nonblocking;
    sync::spin_unlock_irqrestore(socket->lock, irq);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void add_ref(stream_socket* socket) {
    if (!socket) {
        return;
    }
    socket->add_ref();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void release(stream_socket* socket) {
    if (!socket) {
        return;
    }
    if (socket->release()) {
        stream_socket::ref_destroy(socket);
    }
}

} // namespace net::unix_stream
