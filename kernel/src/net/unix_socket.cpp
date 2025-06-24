#include <net/unix_socket.h>
#include <net/unix_socket_manager.h>
#include <core/klog.h>
#include <sched/sched.h>
#include <errno.h>

namespace net {

unix_stream_socket::unix_stream_socket() {
    _setup_buffers();
}

unix_stream_socket::~unix_stream_socket() {
    // Only close if not already closed to avoid double-close deadlock
    if (m_state.load() != unix_socket_state::CLOSED) {
        close();
    }
}

void unix_stream_socket::_setup_buffers() {
    m_recv_buffer = kstl::shared_ptr<unix_socket_buffer>(new unix_socket_buffer());
    m_send_buffer = kstl::shared_ptr<unix_socket_buffer>(new unix_socket_buffer());
}

void unix_stream_socket::_change_state(unix_socket_state new_state) {
    unix_socket_state old_state = m_state.exchange(new_state);
    __unused old_state;
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] Socket state changed: %u -> %u\n", 
                      static_cast<uint32_t>(old_state), static_cast<uint32_t>(new_state));
}

bool unix_stream_socket::_can_accept() const {
    return m_state.load() == unix_socket_state::LISTENING && m_is_server;
}

bool unix_stream_socket::_can_read() const {
    unix_socket_state state = m_state.load();
    return state == unix_socket_state::CONNECTED || state == unix_socket_state::DISCONNECTED;
}

bool unix_stream_socket::_can_write() const {
    unix_socket_state state = m_state.load();
    return state == unix_socket_state::CONNECTED;
}

void unix_stream_socket::_cleanup_resources() {
    // NOTE: This method assumes m_socket_lock is already held by the caller
    
    // Clear peer connection
    m_peer = kstl::shared_ptr<unix_stream_socket>(nullptr);
    
    // Clear pending connections
    {
        mutex_guard accept_guard(m_accept_lock);
        m_pending_connections.clear();
    }
    
    // Clear buffers
    if (m_recv_buffer) {
        m_recv_buffer->clear();
    }
    if (m_send_buffer) {
        m_send_buffer->clear();
    }
}

int unix_stream_socket::_add_pending_connection(kstl::shared_ptr<unix_stream_socket> client) {
    mutex_guard guard(m_accept_lock);
    
    if (static_cast<int>(m_pending_connections.size()) >= m_backlog) {
        return -ECONNREFUSED; // Queue is full
    }
    
    m_pending_connections.push_back(client);
    return 0;
}

kstl::shared_ptr<unix_stream_socket> unix_stream_socket::_get_pending_connection() {
    mutex_guard guard(m_accept_lock);
    
    if (m_pending_connections.empty()) {
        return kstl::shared_ptr<unix_stream_socket>(nullptr);
    }
    
    auto client = m_pending_connections[0];
    m_pending_connections.erase(0);
    return client;
}

void unix_stream_socket::_set_peer(kstl::shared_ptr<unix_stream_socket> peer) {
    mutex_guard guard(m_socket_lock);
    m_peer = peer;
}

int unix_stream_socket::bind(const kstl::string& path) {
    mutex_guard guard(m_socket_lock);
    
    if (m_state.load() != unix_socket_state::CREATED) {
        return -EINVAL; // Socket already bound or in use
    }
    
    if (path.empty()) {
        return -EINVAL; // Invalid path
    }
    
    m_path = path;
    m_is_server = true;
    _change_state(unix_socket_state::BOUND);
    
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] Socket bound to path: %s (registration deferred)\n", path.c_str());
    return 0;
}

int unix_stream_socket::listen(int backlog) {
    mutex_guard guard(m_socket_lock);
    
    if (m_state.load() != unix_socket_state::BOUND) {
        return -EINVAL; // Must bind before listening
    }
    
    if (backlog <= 0) {
        backlog = 5; // Default backlog
    }
    
    m_backlog = backlog;
    _change_state(unix_socket_state::LISTENING);
    
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] Socket listening with backlog: %d\n", backlog);
    return 0;
}

kstl::shared_ptr<unix_stream_socket> unix_stream_socket::accept() {
    if (!_can_accept()) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] Error: Cannot accept on this socket\n");
        return kstl::shared_ptr<unix_stream_socket>(nullptr);
    }
    
    // Try to get a pending connection immediately
    auto client = _get_pending_connection();
    if (client) {
        // Create a new socket for this connection
        auto connection_socket = kstl::shared_ptr<unix_stream_socket>(new unix_stream_socket());
        if (!connection_socket) {
            UNIX_SOCKET_TRACE("[UNIX_SOCKET] Error: Failed to create connection socket\n");
            return kstl::shared_ptr<unix_stream_socket>(nullptr);
        }
        
        // Set up the bidirectional connection
        connection_socket->m_peer = client;
        client->m_peer = connection_socket;
    
        // Both sockets are now connected
        connection_socket->_change_state(unix_socket_state::CONNECTED);
        client->_change_state(unix_socket_state::CONNECTED);
        
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] Accepted connection\n");
        return connection_socket;
    }
    
    // No pending connections available
    if (m_nonblocking) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] No pending connections (non-blocking)\n");
        return kstl::shared_ptr<unix_stream_socket>(nullptr); // Will be converted to EAGAIN by fcntl wrapper
    }
    
    // Blocking mode - wait for a connection
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] Waiting for incoming connections (blocking)...\n");
    while (true) {
        client = _get_pending_connection();
        if (client) {
            // Create a new socket for this connection
            auto connection_socket = kstl::shared_ptr<unix_stream_socket>(new unix_stream_socket());
            if (!connection_socket) {
                UNIX_SOCKET_TRACE("[UNIX_SOCKET] Error: Failed to create connection socket\n");
                return kstl::shared_ptr<unix_stream_socket>(nullptr);
            }
            
            // Set up the bidirectional connection
            connection_socket->m_peer = client;
            client->m_peer = connection_socket;
            
            // Both sockets are now connected
            connection_socket->_change_state(unix_socket_state::CONNECTED);
            client->_change_state(unix_socket_state::CONNECTED);
            
            UNIX_SOCKET_TRACE("[UNIX_SOCKET] Accepted connection\n");
            return connection_socket;
        }
        
        // No pending connections, yield and try again
        sched::yield();
        
        // Check if we're still listening
        if (!_can_accept()) {
            UNIX_SOCKET_TRACE("[UNIX_SOCKET] Socket no longer accepting connections\n");
            return kstl::shared_ptr<unix_stream_socket>(nullptr);
        }
    }
}

int unix_stream_socket::connect(const kstl::string& path) {
    mutex_guard guard(m_socket_lock);
    
    if (m_state.load() != unix_socket_state::CREATED) {
        return -EINVAL; // Socket already connected or in use
    }
    
    if (path.empty()) {
        return -EINVAL; // Invalid path
    }
    
    _change_state(unix_socket_state::CONNECTING);
    
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] Attempting to connect to: %s\n", path.c_str());
    
    // Find the server socket via socket manager
    auto& manager = unix_socket_manager::get();
    auto server = manager.find_socket(path);
    if (!server) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] Error: Server socket not found at path: %s\n", path.c_str());
        _change_state(unix_socket_state::CREATED);
        return -ECONNREFUSED;
    }
    
    // For now, directly add ourselves to the server's pending queue
    // The shared_ptr issue will be resolved when sockets are created via manager
    int result = server->_add_pending_connection(kstl::shared_ptr<unix_stream_socket>(this));
    if (result != 0) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] Error: Failed to add to server's pending queue: %d\n", result);
        _change_state(unix_socket_state::CREATED);
        return result;
    }
    
    // For Unix domain sockets, connection is typically instant, but check mode
    if (m_nonblocking) {
        // Non-blocking mode: check if connection completed immediately
        if (m_state.load() == unix_socket_state::CONNECTED) {
            UNIX_SOCKET_TRACE("[UNIX_SOCKET] Successfully connected to: %s (immediate)\n", path.c_str());
            return 0;
        } else {
            // Connection in progress - for Unix sockets this is usually instant
            // but we'll return EINPROGRESS to be semantically correct
            UNIX_SOCKET_TRACE("[UNIX_SOCKET] Connection in progress (non-blocking)\n");
            return -EINPROGRESS;
        }
    }
    
    // Blocking mode: wait for the server to accept us (our state will change to CONNECTED)
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] Waiting for server to accept connection (blocking)...\n");
    while (m_state.load() == unix_socket_state::CONNECTING) {
        sched::yield();
    }
    
    if (m_state.load() == unix_socket_state::CONNECTED) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] Successfully connected to: %s\n", path.c_str());
        return 0;
    } else {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] Connection failed or was rejected\n");
        _change_state(unix_socket_state::CREATED);
        return -ECONNREFUSED;
    }
}

ssize_t unix_stream_socket::read(void* buffer, size_t size) {
    if (!buffer || size == 0) {
        return -EINVAL;
    }
    
    unix_socket_state current_state = m_state.load();
    
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] read() called, socket state: %u\n", static_cast<uint32_t>(current_state));
    
    // If socket is not connected and not disconnected, it's an error
    if (current_state != unix_socket_state::CONNECTED && 
        current_state != unix_socket_state::DISCONNECTED) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] read() returning ENOTCONN for state %u\n", static_cast<uint32_t>(current_state));
        return -ENOTCONN;
    }
    
    if (!m_recv_buffer) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] read() returning EBADF - no recv buffer\n");
        return -EBADF; // Invalid socket state
    }
    
    // Check if we have any data to read
    if (m_recv_buffer->has_data()) {
        // Data is available, read it
        size_t bytes_read = m_recv_buffer->read(buffer, size);
        return static_cast<ssize_t>(bytes_read);
    }
    
    // No data available
    if (current_state == unix_socket_state::DISCONNECTED) {
        // Peer has disconnected and no more data - return EOF
        return 0;
    }
    
    // Still connected but no data available
    if (m_nonblocking) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] No data available (non-blocking)\n");
        return -EAGAIN; // Non-blocking mode: return immediately
    }
    
    // Blocking mode - wait until data arrives or disconnection
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] Blocking until data arrives...\n");
    while (true) {
        current_state = m_state.load();
        
        // Check for disconnection
        if (current_state == unix_socket_state::DISCONNECTED) {
            // Check one more time for any remaining data
            if (m_recv_buffer->has_data()) {
                size_t bytes_read = m_recv_buffer->read(buffer, size);
                return static_cast<ssize_t>(bytes_read);
            }
            return 0; // EOF - peer disconnected and no more data
        }
        
        // Check if connection is broken
        if (current_state != unix_socket_state::CONNECTED) {
            return -ENOTCONN;
        }
        
        // Check for new data
        if (m_recv_buffer->has_data()) {
            size_t bytes_read = m_recv_buffer->read(buffer, size);
            return static_cast<ssize_t>(bytes_read);
        }
        
        // No data available, yield and try again
        sched::yield();
    }
}

ssize_t unix_stream_socket::write(const void* data, size_t size) {
    if (!data || size == 0) {
        return -EINVAL;
    }
    
    if (!_can_write()) {
        return -ENOTCONN;
    }
    
    // Get peer's receive buffer
    auto peer = m_peer;
    if (!peer || !peer->m_recv_buffer) {
        return -EPIPE; // Broken pipe
    }
    
    // Try to write to peer's receive buffer
    size_t bytes_written = peer->m_recv_buffer->write(data, size);
    
    if (bytes_written == 0 && size > 0) {
        // Peer's buffer is full
        if (m_nonblocking) {
            UNIX_SOCKET_TRACE("[UNIX_SOCKET] Peer buffer full (non-blocking)\n");
            return -EAGAIN; // Non-blocking mode: return immediately
        } else {
            // Blocking mode: for now, we'll return 0 to indicate no bytes written
            // In a full implementation, we might want to block until space is available
            UNIX_SOCKET_TRACE("[UNIX_SOCKET] Warning: Peer buffer full, no bytes written (blocking)\n");
        return 0;
        }
    }
    
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] Wrote %llu bytes to peer\n", bytes_written);
    return static_cast<ssize_t>(bytes_written);
}

int unix_stream_socket::close() {
    mutex_guard guard(m_socket_lock);
    
    unix_socket_state current_state = m_state.load();
    if (current_state == unix_socket_state::CLOSED) {
        return 0; // Already closed
    }
    
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] Closing socket (path: %s)\n", 
           m_path.empty() ? "none" : m_path.c_str());
    
    // Unregister from manager if we're a bound server socket
    if (m_is_server && !m_path.empty()) {
        auto& manager = unix_socket_manager::get();
        manager.unregister_socket(m_path);
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] Unregistered socket from manager\n");
    }
    
    // Notify peer of disconnection
    if (m_peer) {
        m_peer->_change_state(unix_socket_state::DISCONNECTED);
        m_peer->m_peer = kstl::shared_ptr<unix_stream_socket>(nullptr);
                 UNIX_SOCKET_TRACE("[UNIX_SOCKET] Notified peer of disconnection\n");
    }
    
    _cleanup_resources();
    _change_state(unix_socket_state::CLOSED);
    
    return 0;
}

int unix_stream_socket::register_with_manager(kstl::shared_ptr<unix_stream_socket> self) {
    if (!m_is_server || m_path.empty()) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] Error: Only bound server sockets can be registered\n");
        return -EINVAL;
    }
    
    if (m_state.load() != unix_socket_state::BOUND) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] Error: Socket must be bound before registration\n");
        return -EINVAL;
    }
    
    auto& manager = unix_socket_manager::get();
    if (!manager.register_socket(m_path, self)) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET] Error: Failed to register socket with manager\n");
        return -EADDRINUSE;
    }
    
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] Socket registered with manager at path: %s\n", m_path.c_str());
    return 0;
}

int unix_stream_socket::set_nonblocking(bool nonblocking) {
    mutex_guard guard(m_socket_lock);
    
    m_nonblocking = nonblocking;
    
    UNIX_SOCKET_TRACE("[UNIX_SOCKET] Socket set to %s mode\n", 
                      nonblocking ? "non-blocking" : "blocking");
    return 0;
}

} // namespace net 