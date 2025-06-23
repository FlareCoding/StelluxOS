#ifndef UNIX_SOCKET_H
#define UNIX_SOCKET_H

#include <core/sync.h>
#include <core/types.h>
#include <core/string.h>
#include <kstl/vector.h>
#include <memory/memory.h>
#include <net/unix_socket_buffer.h>

// Uncomment the line below to enable verbose Unix socket debugging
// #define STELLUX_UNIX_SOCKET_DEBUG

#ifdef STELLUX_UNIX_SOCKET_DEBUG
#include <core/klog.h>
#define UNIX_SOCKET_TRACE(...) kprint(__VA_ARGS__)
#else
#define UNIX_SOCKET_TRACE(...) do { } while(0)
#endif

namespace net {

/**
 * @enum unix_socket_state
 * @brief Represents the state of a Unix socket.
 */
enum class unix_socket_state : uint32_t {
    INVALID = 0,        // Socket is invalid/uninitialized
    CREATED,            // Socket created but not bound/connected
    BOUND,              // Server socket bound to path
    LISTENING,          // Server socket listening for connections
    CONNECTING,         // Client socket attempting to connect
    CONNECTED,          // Connected socket (client or accepted connection)
    DISCONNECTED,       // Socket disconnected but not closed
    CLOSED              // Socket closed and resources freed
};

/**
 * @class unix_stream_socket
 * @brief Unix domain stream socket implementation.
 * 
 * Provides reliable, ordered, connection-based communication between processes
 * on the same machine using filesystem paths as addresses.
 */
class unix_stream_socket {
public:
    /**
     * @brief Constructs a new Unix stream socket.
     */
    unix_stream_socket();

    /**
     * @brief Destructor - cleans up socket resources.
     */
    ~unix_stream_socket();

    // Non-copyable, non-movable for thread safety
    unix_stream_socket(const unix_stream_socket&) = delete;
    unix_stream_socket& operator=(const unix_stream_socket&) = delete;
    unix_stream_socket(unix_stream_socket&&) = delete;
    unix_stream_socket& operator=(unix_stream_socket&&) = delete;

    /**
     * @brief Binds the socket to a filesystem path.
     * 
     * Creates a socket file at the specified path that clients can connect to.
     * Only server sockets need to bind to a path.
     * 
     * @param path Filesystem path to bind to (e.g., "/tmp/my_socket")
     * @return 0 on success, negative error code on failure
     */
    int bind(const kstl::string& path);

    /**
     * @brief Puts the socket into listening mode.
     * 
     * Marks the socket as passive, ready to accept incoming connections.
     * Must be called after bind() and before accept().
     * 
     * @param backlog Maximum number of pending connections (default: 5)
     * @return 0 on success, negative error code on failure
     */
    int listen(int backlog = 5);

    /**
     * @brief Accepts an incoming connection (blocking).
     * 
     * Blocks until a client connects, then returns a new socket for that connection.
     * The original socket remains in listening state for more connections.
     * 
     * @return Shared pointer to new socket for the connection, or nullptr on error
     */
    kstl::shared_ptr<unix_stream_socket> accept();

    /**
     * @brief Connects to a server socket (blocking).
     * 
     * Attempts to establish a connection to a server socket at the given path.
     * Blocks until connection is established or fails.
     * 
     * @param path Path to connect to
     * @return 0 on success, negative error code on failure
     */
    int connect(const kstl::string& path);

    /**
     * @brief Reads data from the socket (blocking).
     * 
     * Blocks until data is available or the connection is closed.
     * For stream sockets, may return fewer bytes than requested.
     * 
     * @param buffer Buffer to read data into
     * @param size Maximum number of bytes to read
     * @return Number of bytes read, 0 for EOF, negative for error
     */
    ssize_t read(void* buffer, size_t size);

    /**
     * @brief Writes data to the socket.
     * 
     * Attempts to write data to the connected peer. May write fewer bytes
     * than requested if the peer's receive buffer is full.
     * 
     * @param data Data to write
     * @param size Number of bytes to write
     * @return Number of bytes written, negative for error
     */
    ssize_t write(const void* data, size_t size);

    /**
     * @brief Closes the socket.
     * 
     * Closes the socket and releases all associated resources.
     * Any pending operations will be interrupted.
     * 
     * @return 0 on success, negative error code on failure
     */
    int close();

    /**
     * @brief Registers this socket with the manager (for server sockets).
     * 
     * Must be called after bind() for server sockets to make them discoverable.
     * This is a separate step to avoid shared_ptr issues in bind().
     * 
     * @param self Shared pointer to this socket instance
     * @return 0 on success, negative error code on failure
     */
    int register_with_manager(kstl::shared_ptr<unix_stream_socket> self);

    // State and property getters
    unix_socket_state get_state() const { return m_state.load(); }
    const kstl::string& get_path() const { return m_path; }
    bool is_server() const { return m_is_server; }
    bool is_connected() const { return get_state() == unix_socket_state::CONNECTED; }
    bool is_listening() const { return get_state() == unix_socket_state::LISTENING; }

private:
    // Core socket state
    atomic<unix_socket_state> m_state = atomic<unix_socket_state>(unix_socket_state::CREATED);
    kstl::string m_path;                                        // Bound path (for server sockets)
    bool m_is_server = false;                                   // True if this is a server socket
    int m_backlog = 0;                                          // Listen backlog size
    
    // Connection management
    kstl::shared_ptr<unix_stream_socket> m_peer;               // Connected peer socket
    kstl::vector<kstl::shared_ptr<unix_stream_socket>> m_pending_connections; // Pending accept queue
    
    // Data buffers
    kstl::shared_ptr<unix_socket_buffer> m_recv_buffer;        // Incoming data buffer
    kstl::shared_ptr<unix_socket_buffer> m_send_buffer;        // Outgoing data buffer
    
    // Synchronization
    mutable mutex m_socket_lock = mutex();                     // Protects socket state
    mutex m_accept_lock = mutex();                             // Protects pending connections
    
    // Private helper methods
    void _change_state(unix_socket_state new_state);
    bool _can_accept() const;
    bool _can_read() const;
    bool _can_write() const;
    void _cleanup_resources();
    void _setup_buffers();
    int _add_pending_connection(kstl::shared_ptr<unix_stream_socket> client);
    kstl::shared_ptr<unix_stream_socket> _get_pending_connection();
    void _set_peer(kstl::shared_ptr<unix_stream_socket> peer);
    
    // Allow manager to access private members for connection setup
    friend class unix_socket_manager;
};

} // namespace net

#endif // UNIX_SOCKET_H

