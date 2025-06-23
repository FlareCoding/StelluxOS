#include <net/unix_socket_manager.h>
#include <net/unix_socket.h>
#include <core/klog.h>
#include <errno.h>

namespace net {

// Error codes for Unix socket manager
#define ENOTINIT        125  // Manager not initialized
#define EADDRINUSE      98   // Address already in use
#define ENOENT          2    // No such file or directory
#define EINVAL          22   // Invalid argument
#define ECONNREFUSED    111  // Connection refused

unix_socket_manager unix_socket_manager::s_singleton;

unix_socket_manager& unix_socket_manager::get() {
    return s_singleton;
}

void unix_socket_manager::init() {
    mutex_guard guard(m_manager_lock);
    
    if (m_initialized) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Warning: Manager already initialized\n");
        return;
    }
    
    // Initialize the hashmap
    m_bound_sockets = kstl::hashmap<kstl::string, kstl::shared_ptr<unix_stream_socket>>();
    
    m_initialized = true;
}

bool unix_socket_manager::register_socket(const kstl::string& path, 
                                        kstl::shared_ptr<unix_stream_socket> socket) {
    if (!m_initialized) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Error: Manager not initialized\n");
        return false;
    }
    
    if (!socket) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Error: Cannot register null socket\n");
        return false;
    }
    
    if (!_is_valid_path(path)) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Error: Invalid path: %s\n", path.c_str());
        return false;
    }
    
    mutex_guard guard(m_manager_lock);
    
    // Check if path is already in use
    if (m_bound_sockets.find(path)) {
        _log_socket_operation("register", path, false);
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Error: Path already in use: %s\n", path.c_str());
        return false;
    }
    
    // Register the socket
    bool success = m_bound_sockets.insert(path, socket);
    _log_socket_operation("register", path, success);

    if (!success) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Error: Failed to register socket at path: %s\n", path.c_str());
    }
    
    return success;
}

bool unix_socket_manager::unregister_socket(const kstl::string& path) {
    if (!m_initialized) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Error: Manager not initialized\n");
        return false;
    }
    
    if (!_is_valid_path(path)) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Error: Invalid path: %s\n", path.c_str());
        return false;
    }
    
    mutex_guard guard(m_manager_lock);
    
    bool success = m_bound_sockets.remove(path);
    _log_socket_operation("unregister", path, success);

    if (!success) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Warning: Path not found for unregistration: %s\n", path.c_str());
    }
    
    return success;
}

kstl::shared_ptr<unix_stream_socket> unix_socket_manager::find_socket(const kstl::string& path) {
    if (!m_initialized) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Error: Manager not initialized\n");
        return kstl::shared_ptr<unix_stream_socket>(nullptr);
    }
    
    if (!_is_valid_path(path)) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Error: Invalid path: %s\n", path.c_str());
        return kstl::shared_ptr<unix_stream_socket>(nullptr);
    }
    
    mutex_guard guard(m_manager_lock);
    
    auto* socket_ptr = m_bound_sockets.get(path);
    if (socket_ptr) {
        _log_socket_operation("find", path, true);
        return *socket_ptr;
    }
    
    _log_socket_operation("find", path, false);
    return kstl::shared_ptr<unix_stream_socket>(nullptr);
}

kstl::shared_ptr<unix_stream_socket> unix_socket_manager::create_socket() {
    if (!m_initialized) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Error: Manager not initialized\n");
        return kstl::shared_ptr<unix_stream_socket>(nullptr);
    }
    
    // Create a new socket instance
    auto socket = kstl::shared_ptr<unix_stream_socket>(new unix_stream_socket());
    if (!socket) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Error: Failed to create socket\n");
        return kstl::shared_ptr<unix_stream_socket>(nullptr);
    }

    return socket;
}

size_t unix_socket_manager::get_socket_count() const {
    if (!m_initialized) {
        return 0;
    }
    
    mutex_guard guard(m_manager_lock);
    return m_bound_sockets.size();
}

void unix_socket_manager::cleanup() {
    mutex_guard guard(m_manager_lock);
    
    if (!m_initialized) {
        UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] Warning: Manager not initialized, nothing to cleanup\n");
        return;
    }
    
    // Get all keys before clearing (to avoid iterator invalidation)
    auto paths = m_bound_sockets.keys();
    
    // Close all registered sockets
    for (const auto& path : paths) {
        auto* socket_ptr = m_bound_sockets.get(path);
        if (socket_ptr && *socket_ptr) {
            (*socket_ptr)->close();
        }
    }
    
    // Clear the hashmap
    m_bound_sockets = kstl::hashmap<kstl::string, kstl::shared_ptr<unix_stream_socket>>();
    
    m_initialized = false;
}

bool unix_socket_manager::_is_valid_path(const kstl::string& path) const {
    // Basic path validation
    if (path.empty()) {
        return false;
    }
    
    // Unix socket paths should start with '/' for absolute paths
    // or be relative paths (not starting with '/')
    // For now, accept any non-empty path
    const char* path_str = path.c_str();
    size_t len = strlen(path_str);
    
    // Check for reasonable length limits
    if (len > 255) { // Typical filesystem path limit
        return false;
    }
    
    // Check for null bytes within the path
    for (size_t i = 0; i < len; i++) {
        if (path_str[i] == '\0') {
            return false;
        }
    }
    
    return true;
}

void unix_socket_manager::_log_socket_operation(const char* operation, 
                                               const kstl::string& path, 
                                               bool success) const {
    const char* status = success ? "SUCCESS" : "FAILED";
    __unused status; __unused operation; __unused path;
    UNIX_SOCKET_TRACE("[UNIX_SOCKET_MGR] %s operation %s for path: %s\n", 
           operation, status, path.c_str());
}

} // namespace net 