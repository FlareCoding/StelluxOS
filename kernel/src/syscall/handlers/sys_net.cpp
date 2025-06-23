#include <syscall/handlers/sys_net.h>
#include <net/unix_socket_manager.h>
#include <process/process.h>
#include <core/klog.h>

// Socket address structures
struct sockaddr_un {
    uint16_t sun_family;  // AF_UNIX
    char sun_path[108];   // pathname
};

// Socket family constants
#define AF_UNIX 1

// Socket type constants  
#define SOCK_STREAM 1

DECLARE_SYSCALL_HANDLER(socket) {
    /* -----------------------------------------------------------------
     *  Linux-style calling convention
     *      arg1 = int           domain    (AF_UNIX)
     *      arg2 = int           type      (SOCK_STREAM)
     *      arg3 = int           protocol  (0)
     * -----------------------------------------------------------------*/
    
    int domain = static_cast<int>(arg1);
    int type = static_cast<int>(arg2);
    int protocol = static_cast<int>(arg3);
    
    // Validate socket parameters
    if (domain != AF_UNIX) {
        SYSCALL_TRACE("socket(%d, %d, %d) = -EAFNOSUPPORT\n", domain, type, protocol);
        return -EAFNOSUPPORT; // Address family not supported
    }
    
    if (type != SOCK_STREAM) {
        SYSCALL_TRACE("socket(%d, %d, %d) = -EPROTONOSUPPORT\n", domain, type, protocol);
        return -EPROTONOSUPPORT; // Protocol not supported
    }
    
    if (protocol != 0) {
        SYSCALL_TRACE("socket(%d, %d, %d) = -EPROTONOSUPPORT\n", domain, type, protocol);
        return -EPROTONOSUPPORT; // Protocol not supported
    }
    
    // Create socket via manager
    auto& manager = net::unix_socket_manager::get();
    auto socket = manager.create_socket();
    if (!socket) {
        SYSCALL_TRACE("socket(%d, %d, %d) = -ENOMEM\n", domain, type, protocol);
        return -ENOMEM;
    }
    
    // Create shared_ptr wrapper for handle storage
    auto* socket_ptr = new kstl::shared_ptr<net::unix_stream_socket>(socket);
    if (!socket_ptr) {
        SYSCALL_TRACE("socket(%d, %d, %d) = -ENOMEM\n", domain, type, protocol);
        return -ENOMEM;
    }
    
    // Add to process handle table
    auto& handles = current->get_env()->handles;
    size_t handle = handles.add_handle(handle_type::UNIX_SOCKET, socket_ptr);
    if (handle == SIZE_MAX) {
        delete socket_ptr;
        SYSCALL_TRACE("socket(%d, %d, %d) = -EMFILE\n", domain, type, protocol);
        return -EMFILE; // Too many open files
    }
    
    SYSCALL_TRACE("socket(%d, %d, %d) = %d\n", domain, type, protocol, static_cast<int>(handle));
    return static_cast<long>(handle);
}

DECLARE_SYSCALL_HANDLER(bind) {
    /* -----------------------------------------------------------------
     *  Linux-style calling convention
     *      arg1 = int                    sockfd
     *      arg2 = const struct sockaddr* addr
     *      arg3 = socklen_t              addrlen
     * -----------------------------------------------------------------*/
    
    int sockfd = static_cast<int>(arg1);
    const sockaddr_un* addr = reinterpret_cast<const sockaddr_un*>(arg2);
    uint32_t addrlen = static_cast<uint32_t>(arg3);
    
    // Validate parameters
    if (!addr) {
        return -EFAULT;
    }
    
    if (addrlen < sizeof(sockaddr_un) || addr->sun_family != AF_UNIX) {
        return -EINVAL;
    }
    
    // Get socket handle
    handle_entry* hentry = current->get_env()->handles.get_handle(sockfd);
    if (!hentry || hentry->type != handle_type::UNIX_SOCKET) {
        return -EBADF;
    }
    
    auto* socket_ptr = static_cast<kstl::shared_ptr<net::unix_stream_socket>*>(hentry->object);
    if (!socket_ptr || !*socket_ptr) {
        return -EBADF;
    }
    
    // Extract path from address
    kstl::string path(addr->sun_path);
    
    // Bind socket
    int result = (*socket_ptr)->bind(path);
    if (result == 0) {
        // Register with manager after successful bind
        int reg_result = (*socket_ptr)->register_with_manager(*socket_ptr);
        if (reg_result != 0) {
            (*socket_ptr)->close(); // Clean up on registration failure
            result = reg_result;
        }
    }
    
    SYSCALL_TRACE("bind(%d, {sun_family=AF_UNIX, sun_path=\"%s\"}, %u) = %d\n", 
                  sockfd, addr->sun_path, addrlen, result);
    return result;
}

DECLARE_SYSCALL_HANDLER(listen) {
    /* -----------------------------------------------------------------
     *  Linux-style calling convention
     *      arg1 = int           sockfd
     *      arg2 = int           backlog
     * -----------------------------------------------------------------*/
    
    int sockfd = static_cast<int>(arg1);
    int backlog = static_cast<int>(arg2);
    
    // Get socket handle
    handle_entry* hentry = current->get_env()->handles.get_handle(sockfd);
    if (!hentry || hentry->type != handle_type::UNIX_SOCKET) {
        return -EBADF;
    }
    
    auto* socket_ptr = static_cast<kstl::shared_ptr<net::unix_stream_socket>*>(hentry->object);
    if (!socket_ptr || !*socket_ptr) {
        return -EBADF;
    }
    
    // Listen on socket
    int result = (*socket_ptr)->listen(backlog);
    
    SYSCALL_TRACE("listen(%d, %d) = %d\n", sockfd, backlog, result);
    return result;
}

DECLARE_SYSCALL_HANDLER(accept) {
    /* -----------------------------------------------------------------
     *  Linux-style calling convention
     *      arg1 = int                    sockfd
     *      arg2 = struct sockaddr*       addr     (optional, can be NULL)
     *      arg3 = socklen_t*             addrlen  (optional, can be NULL)
     * -----------------------------------------------------------------*/
    
    int sockfd = static_cast<int>(arg1);
    sockaddr_un* addr = reinterpret_cast<sockaddr_un*>(arg2);
    uint32_t* addrlen = reinterpret_cast<uint32_t*>(arg3);
    
    // Get socket handle
    handle_entry* hentry = current->get_env()->handles.get_handle(sockfd);
    if (!hentry || hentry->type != handle_type::UNIX_SOCKET) {
        return -EBADF;
    }
    
    auto* socket_ptr = static_cast<kstl::shared_ptr<net::unix_stream_socket>*>(hentry->object);
    if (!socket_ptr || !*socket_ptr) {
        return -EBADF;
    }
    
    // Accept connection (blocking)
    auto client_socket = (*socket_ptr)->accept();
    if (!client_socket) {
        SYSCALL_TRACE("accept(%d, %p, %p) = -EINVAL\n", sockfd, addr, addrlen);
        return -EINVAL;
    }
    
    // Create shared_ptr wrapper for the new connection
    auto* client_socket_ptr = new kstl::shared_ptr<net::unix_stream_socket>(client_socket);
    if (!client_socket_ptr) {
        SYSCALL_TRACE("accept(%d, %p, %p) = -ENOMEM\n", sockfd, addr, addrlen);
        return -ENOMEM;
    }
    
    // Add to process handle table
    auto& handles = current->get_env()->handles;
    size_t client_handle = handles.add_handle(handle_type::UNIX_SOCKET, client_socket_ptr);
    if (client_handle == SIZE_MAX) {
        delete client_socket_ptr;
        SYSCALL_TRACE("accept(%d, %p, %p) = -EMFILE\n", sockfd, addr, addrlen);
        return -EMFILE;
    }
    
    // Fill in client address if requested
    if (addr && addrlen) {
        addr->sun_family = AF_UNIX;
        addr->sun_path[0] = '\0'; // Anonymous socket for client
        *addrlen = sizeof(sockaddr_un);
    }
    
    SYSCALL_TRACE("accept(%d, %p, %p) = %d\n", sockfd, addr, addrlen, static_cast<int>(client_handle));
    return static_cast<long>(client_handle);
}

DECLARE_SYSCALL_HANDLER(connect) {
    /* -----------------------------------------------------------------
     *  Linux-style calling convention
     *      arg1 = int                    sockfd
     *      arg2 = const struct sockaddr* addr
     *      arg3 = socklen_t              addrlen
     * -----------------------------------------------------------------*/
    
    int sockfd = static_cast<int>(arg1);
    const sockaddr_un* addr = reinterpret_cast<const sockaddr_un*>(arg2);
    uint32_t addrlen = static_cast<uint32_t>(arg3);
    
    // Validate parameters
    if (!addr) {
        return -EFAULT;
    }
    
    if (addrlen < sizeof(sockaddr_un) || addr->sun_family != AF_UNIX) {
        return -EINVAL;
    }
    
    // Get socket handle
    handle_entry* hentry = current->get_env()->handles.get_handle(sockfd);
    if (!hentry || hentry->type != handle_type::UNIX_SOCKET) {
        return -EBADF;
    }
    
    auto* socket_ptr = static_cast<kstl::shared_ptr<net::unix_stream_socket>*>(hentry->object);
    if (!socket_ptr || !*socket_ptr) {
        return -EBADF;
    }
    
    // Extract path from address
    kstl::string path(addr->sun_path);
    
    // Connect to server
    int result = (*socket_ptr)->connect(path);
    
    SYSCALL_TRACE("connect(%d, {sun_family=AF_UNIX, sun_path=\"%s\"}, %u) = %d\n", 
                  sockfd, addr->sun_path, addrlen, result);
    return result;
} 