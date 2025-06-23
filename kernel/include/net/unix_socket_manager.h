#ifndef UNIX_SOCKET_MANAGER_H
#define UNIX_SOCKET_MANAGER_H

#include <core/string.h>
#include <kstl/hashmap.h>
#include <memory/memory.h>
#include <net/unix_socket.h>

namespace net {
/**
 * @class unix_socket_manager
 * @brief Manages Unix domain sockets and their filesystem bindings.
 * 
 * Singleton class that handles socket registration, lookup, and connection
 * establishment between client and server sockets. Provides a centralized
 * registry for all Unix domain sockets in the system.
 */
class unix_socket_manager {
public:
    /**
     * @brief Gets the singleton instance of the socket manager.
     * @return Reference to the singleton instance
     */
    static unix_socket_manager& get();

    /**
     * @brief Initializes the socket manager.
     * 
     * Must be called once during kernel initialization before using sockets.
     * Sets up internal data structures.
     */
    void init();

    /**
     * @brief Registers a socket with a filesystem path.
     * 
     * Associates a server socket with a filesystem path so clients can find it.
     * The path must be unique - only one socket can be bound to each path.
     * 
     * @param path Filesystem path (e.g., "/tmp/my_socket")
     * @param socket Socket to register
     * @return True on success, false if path already exists or manager not initialized
     */
    bool register_socket(const kstl::string& path, kstl::shared_ptr<unix_stream_socket> socket);

    /**
     * @brief Unregisters a socket from a filesystem path.
     * 
     * Removes the association between a path and socket, making the path
     * available for other sockets to bind to.
     * 
     * @param path Filesystem path to unregister
     * @return True on success, false if path not found or manager not initialized
     */
    bool unregister_socket(const kstl::string& path);

    /**
     * @brief Finds a socket by filesystem path.
     * 
     * Looks up a server socket that is bound to the specified path.
     * Used by clients to find servers to connect to.
     * 
     * @param path Filesystem path to search for
     * @return Socket pointer or nullptr if not found or manager not initialized
     */
    kstl::shared_ptr<unix_stream_socket> find_socket(const kstl::string& path);

    /**
     * @brief Creates a new Unix stream socket.
     * 
     * Factory method that creates a properly initialized socket instance.
     * 
     * @return New socket instance
     */
    kstl::shared_ptr<unix_stream_socket> create_socket();

    /**
     * @brief Gets the number of registered sockets.
     * @return Number of sockets currently registered with paths
     */
    size_t get_socket_count() const;

    /**
     * @brief Checks if the manager is initialized.
     * @return True if initialized, false otherwise
     */
    bool is_initialized() const { return m_initialized; }

    /**
     * @brief Cleans up all registered sockets.
     * 
     * Used during system shutdown to clean up all socket resources.
     * Should only be called during kernel shutdown.
     */
    void cleanup();

private:
    static unix_socket_manager s_singleton;

    unix_socket_manager() = default;
    
    // Non-copyable, non-movable
    unix_socket_manager(const unix_socket_manager&) = delete;
    unix_socket_manager& operator=(const unix_socket_manager&) = delete;
    unix_socket_manager(unix_socket_manager&&) = delete;
    unix_socket_manager& operator=(unix_socket_manager&&) = delete;
    
    bool m_initialized = false;                                 // Manager initialization state
    kstl::hashmap<kstl::string, kstl::shared_ptr<unix_stream_socket>> m_bound_sockets; // Path -> Socket mapping
    mutable mutex m_manager_lock = mutex();                     // Protects manager state
    
    // Private helper methods
    bool _is_valid_path(const kstl::string& path) const;
    void _log_socket_operation(const char* operation, const kstl::string& path, bool success) const;
};
} // namespace net

#endif // UNIX_SOCKET_MANAGER_H

