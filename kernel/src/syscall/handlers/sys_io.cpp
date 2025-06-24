#include <syscall/handlers/sys_io.h>
#include <fs/vfs.h>
#include <fs/file_object.h>
#include <fs/filesystem.h>
#include <process/process.h>
#include <core/klog.h>
#include <net/unix_socket.h>
#include <net/unix_socket_manager.h>

DECLARE_SYSCALL_HANDLER(write) {
    int handle = static_cast<int>(arg1);
    const char* buf = reinterpret_cast<const char*>(arg2);
    size_t count = static_cast<size_t>(arg3);
    
    // Validate buffer pointer
    if (!buf) {
        return -EFAULT;
    }
    
    // Get handle entry from process handle table
    handle_entry* hentry = current->get_env()->handles.get_handle(handle);
    
    if (!handle) {
        return -EBADF; // Bad file descriptor
    }
    
    size_t bytes_written = 0;
    
    switch (hentry->type) {
        case handle_type::OUTPUT_STREAM: {
            // Write to serial port (stdout behavior)
            char hack[512] = { 0 };
            memcpy(hack, buf, count < 512 ? count : 511);
            kprint(hack);
            // serial::write(serial::g_kernel_uart_port, buf, count);
            bytes_written = count;
            break;
        }
        case handle_type::FILE: {
            // Write to file
            fs::file_object* fobj = static_cast<fs::file_object*>(hentry->object);
            if (!fobj || !fobj->can_write()) {
                return -EBADF;
            }
            
            // Use VFS write operation
            if (!fobj->vnode || !fobj->vnode->ops.write) {
                return -EIO;
            }
            
            bytes_written = fobj->vnode->ops.write(fobj->vnode.get(), buf, count, fobj->position);
            
            // Update file position on successful write
            if (bytes_written > 0) {
                fobj->position += bytes_written;
            }
            break;
        }
        case handle_type::UNIX_SOCKET: {
            // Write to Unix domain socket
            auto socket_ptr = reinterpret_cast<kstl::shared_ptr<net::unix_stream_socket>*>(hentry->object);
            if (!socket_ptr || !(*socket_ptr)) {
                return -EBADF;
            }
            
            auto socket = *socket_ptr;
            
            // Let the socket's write method handle state checking
            int result = socket->write(buf, count);
            if (result < 0) {
                return result; // Error (already negative)
            }
            
            bytes_written = static_cast<size_t>(result);
            break;
        }
        default: {
            return -EBADF; // Unsupported handle type
        }
    }
    
    SYSCALL_TRACE("write(%i, \"0x%llx\", %llu) = %llu\n", handle, reinterpret_cast<uint64_t>(buf), count, bytes_written);

    return static_cast<long>(bytes_written);
}

DECLARE_SYSCALL_HANDLER(read) {
    /* -----------------------------------------------------------------
     *  Linux-style calling convention
     *      arg1 = int           fd/handle
     *      arg2 = void *        buf       (user pointer)
     *      arg3 = size_t        count
     * -----------------------------------------------------------------*/
    
    int handle = static_cast<int>(arg1);
    void* buf = reinterpret_cast<void*>(arg2);
    size_t count = static_cast<size_t>(arg3);
    
    // Validate buffer pointer
    if (!buf) {
        return -EFAULT;
    }
    
    // Get handle entry from process handle table
    handle_entry* hentry = current->get_env()->handles.get_handle(handle);
    
    if (!hentry) {
        return -EBADF; // Bad file descriptor
    }
    
    ssize_t bytes_read = 0;
    
    switch (hentry->type) {
        case handle_type::INPUT_STREAM: {
            // Read from input source (stdin behavior)
            // For now, return 0 (EOF) since we don't have keyboard input yet
            bytes_read = 0;
            break;
        }
        
        case handle_type::FILE: {
            // Read from file
            fs::file_object* fobj = static_cast<fs::file_object*>(hentry->object);
            if (!fobj || !fobj->can_read()) {
                return -EBADF;
            }
            
            // Use VFS read operation
            if (!fobj->vnode || !fobj->vnode->ops.read) {
                return -EIO;
            }
            
            bytes_read = fobj->vnode->ops.read(fobj->vnode.get(), buf, count, fobj->position);
            
            // Update file position on successful read
            if (bytes_read > 0) {
                fobj->position += bytes_read;
            }
            break;
        }
        
        case handle_type::UNIX_SOCKET: {
            // Read from Unix domain socket
            auto socket_ptr = reinterpret_cast<kstl::shared_ptr<net::unix_stream_socket>*>(hentry->object);
            if (!socket_ptr || !(*socket_ptr)) {
                return -EBADF;
            }
            
            auto socket = *socket_ptr;
            
            // Let the socket's read method handle state checking and EOF logic
            // It can read from both CONNECTED and DISCONNECTED states appropriately
            int result = socket->read(buf, count);
            if (result < 0) {
                return result;
            }
            
            bytes_read = static_cast<ssize_t>(result);
            break;
        }
        
        default: {
            return -EBADF; // Unsupported handle type
        }
    }
    
    SYSCALL_TRACE("read(%d, 0x%llx, %llu) = %lli\n", handle, reinterpret_cast<uint64_t>(buf), count, bytes_read);
    
    return static_cast<long>(bytes_read);
}

DECLARE_SYSCALL_HANDLER(open) {
    /* -----------------------------------------------------------------
     *  Linux-style calling convention
     *      arg1 = const char *  pathname    (user pointer)
     *      arg2 = int           flags       (O_RDONLY, O_WRONLY, etc.)
     *      arg3 = mode_t        mode        (permissions for O_CREAT)
     * -----------------------------------------------------------------*/
    
    const char* pathname = reinterpret_cast<const char*>(arg1);
    uint32_t flags = static_cast<uint32_t>(arg2);
    uint32_t mode = static_cast<uint32_t>(arg3);
    
    // Validate userspace pointer
    if (!pathname) {
        return -EFAULT;
    }
    
    auto& vfs = fs::virtual_filesystem::get();
    
    // Handle O_CREAT logic - create file if it doesn't exist
    if (flags & O_CREAT) {
        if (!vfs.path_exists(pathname)) {
            fs::fs_error create_result = vfs.create(pathname, fs::vfs_node_type::file, mode);
            if (create_result != fs::fs_error::success) {
                return -EACCES; // Failed to create file
            }
        } else if (flags & O_EXCL) {
            // File exists but O_EXCL was specified - fail
            return -EEXIST;
        }
    }
    
    // Resolve path to get the actual vfs_node
    kstl::shared_ptr<fs::vfs_node> node;
    fs::fs_error resolve_result = vfs.resolve_path(pathname, node);
    if (resolve_result != fs::fs_error::success) {
        switch (resolve_result) {
            case fs::fs_error::not_found:
                return -ENOENT;
            case fs::fs_error::invalid_path:
                return -ENOENT;
            case fs::fs_error::not_a_file:
                return -EISDIR;
            default:
                return -EIO;
        }
    }
    
    // Ensure we have a file, not a directory
    if (node->stat.type != fs::vfs_node_type::file) {
        return -EISDIR;
    }
    
    // TODO: Handle O_TRUNC logic - truncate file to 0 if writable
    // if (flags & O_TRUNC && ((flags & O_WRONLY) || (flags & O_RDWR))) {
    //     // Truncate file to 0 length
    // }
    
    // Create file_object with the resolved vfs_node
    fs::file_object* fobj = new fs::file_object();
    if (!fobj) {
        return -ENOMEM;
    }
    
    // Set the fields properly
    fobj->vnode = node;
    fobj->open_flags = flags;
    fobj->status_flags = flags & (O_APPEND | O_NONBLOCK);
    fobj->position = 0;
    
    // If O_APPEND is set, position at end of file
    if (flags & O_APPEND) {
        fobj->position = node->stat.size;
    }
    
    // Add to process handle table
    auto& handles = current->get_env()->handles;
    size_t handle = handles.add_handle(handle_type::FILE, fobj);
    if (handle == SIZE_MAX) {
        delete fobj;
        return -EMFILE; // Too many open files
    }
    
    SYSCALL_TRACE("open(\"%s\", 0x%x, %d) = %d\n",
        pathname, flags, mode, static_cast<int>(handle));
    
    return static_cast<long>(handle);
}

DECLARE_SYSCALL_HANDLER(close) {
    /* -----------------------------------------------------------------
     *  Linux-style calling convention
     *      arg1 = int           fd/handle
     * -----------------------------------------------------------------*/
    
    int handle = static_cast<int>(arg1);
    
    // Get handle entry from process handle table
    auto& handles = current->get_env()->handles;
    handle_entry* hentry = handles.get_handle(handle);
    
    if (!hentry) {
        return -EBADF; // Bad file descriptor
    }
    
    // Handle cleanup based on type
    switch (hentry->type) {
        case handle_type::INPUT_STREAM:
        case handle_type::OUTPUT_STREAM: {
            // Don't allow closing stdin/stdout/stderr (handles 0, 1, 2)
            if (handle <= 2) {
                return -EBADF; // Cannot close standard streams
            }
            // For other stream handles, just remove from table
            break;
        }
        case handle_type::FILE: {
            // Clean up file object
            fs::file_object* fobj = static_cast<fs::file_object*>(hentry->object);
            if (fobj) {
                // The file_object destructor will handle vfs_node cleanup via shared_ptr
                delete fobj;
            }
            break;
        }
        case handle_type::UNIX_SOCKET: {
            // Clean up Unix domain socket
            auto socket_ptr = reinterpret_cast<kstl::shared_ptr<net::unix_stream_socket>*>(hentry->object);
            if (socket_ptr) {
                auto socket = *socket_ptr;
                if (socket) {
                    // Close the socket - this will disconnect peers and clean up state
                    // The socket's close() method handles manager unregistration automatically
                    socket->close();
                }
                
                // Clean up the shared_ptr object itself
                delete socket_ptr;
            }
            break;
        }
        default: {
            return -EBADF; // Invalid handle type
        }
    }
    
    // Remove handle from the table
    if (!handles.remove_handle(handle)) {
        return -EBADF; // Failed to remove handle
    }
    
    SYSCALL_TRACE("close(%i) = 0\n", handle);
    
    return 0; // Success
}

DECLARE_SYSCALL_HANDLER(fcntl) {
    /* -----------------------------------------------------------------
     *  Linux-style calling convention
     *      arg1 = int           fd/handle
     *      arg2 = int           cmd        (F_GETFL, F_SETFL, etc.)
     *      arg3 = long          arg        (flags for F_SETFL)
     * -----------------------------------------------------------------*/
    
    int handle = static_cast<int>(arg1);
    int cmd = static_cast<int>(arg2);
    long fcntl_arg = static_cast<long>(arg3);
    
    // Get handle entry from process handle table
    handle_entry* hentry = current->get_env()->handles.get_handle(handle);
    
    if (!hentry) {
        return -EBADF; // Bad file descriptor
    }
    
    // For now, we only support fcntl on Unix sockets for O_NONBLOCK
    if (hentry->type != handle_type::UNIX_SOCKET) {
        return -ENOTTY; // Not supported on this handle type
    }
    
    auto socket_ptr = reinterpret_cast<kstl::shared_ptr<net::unix_stream_socket>*>(hentry->object);
    if (!socket_ptr || !(*socket_ptr)) {
        return -EBADF;
    }
    
    auto socket = *socket_ptr;
    
    switch (cmd) {
        case 3: { // F_GETFL - get file status flags
            // Return current socket flags
            // For now, we'll track O_NONBLOCK in the socket object
            int flags = socket->is_nonblocking() ? 0x800 : 0; // O_NONBLOCK = 0x800
            SYSCALL_TRACE("fcntl(%d, F_GETFL) = 0x%x\n", handle, flags);
            return flags;
        }
        
        case 4: { // F_SETFL - set file status flags
            // Set socket to blocking/non-blocking based on O_NONBLOCK flag
            int flags = static_cast<int>(fcntl_arg);
            bool nonblocking = (flags & 0x800) != 0; // O_NONBLOCK = 0x800
            
            int result = socket->set_nonblocking(nonblocking);
            if (result != 0) {
                return result;
            }
            
            SYSCALL_TRACE("fcntl(%d, F_SETFL, 0x%x) = 0\n", handle, flags);
            return 0;
        }
        
        default: {
            SYSCALL_TRACE("fcntl(%d, %d, %lli) = -EINVAL (unsupported cmd)\n", handle, cmd, fcntl_arg);
            return -EINVAL; // Invalid command
        }
    }
}

DECLARE_SYSCALL_HANDLER(lseek) {
    /* -----------------------------------------------------------------
     *  Linux-style calling convention
     *      arg1 = int           fd/handle
     *      arg2 = off_t         offset
     *      arg3 = int           whence
     * -----------------------------------------------------------------*/
    
    int handle = static_cast<int>(arg1);
    int64_t offset = static_cast<int64_t>(arg2);
    int whence = static_cast<int>(arg3);
    
    // Get handle entry from process handle table
    handle_entry* hentry = current->get_env()->handles.get_handle(handle);
    
    if (!hentry) {
        return -EBADF; // Bad file descriptor
    }
    
    // Only support seeking on files
    if (hentry->type != handle_type::FILE) {
        return -ESPIPE; // Illegal seek (not a seekable file)
    }
    
    fs::file_object* fobj = static_cast<fs::file_object*>(hentry->object);
    if (!fobj || !fobj->vnode) {
        return -EBADF;
    }
    
    uint64_t new_position = 0;
    uint64_t file_size = fobj->vnode->stat.size;
    
    switch (whence) {
        case SEEK_SET: {
            // Seek from beginning of file
            if (offset < 0) {
                return -EINVAL;
            }
            new_position = static_cast<uint64_t>(offset);
            break;
        }
        
        case SEEK_CUR: {
            // Seek from current position
            int64_t current_pos = static_cast<int64_t>(fobj->position);
            int64_t result_pos = current_pos + offset;
            if (result_pos < 0) {
                return -EINVAL;
            }
            new_position = static_cast<uint64_t>(result_pos);
            break;
        }
        
        case SEEK_END: {
            // Seek from end of file
            int64_t end_pos = static_cast<int64_t>(file_size);
            int64_t result_pos = end_pos + offset;
            if (result_pos < 0) {
                return -EINVAL;
            }
            new_position = static_cast<uint64_t>(result_pos);
            break;
        }
        
        default: {
            return -EINVAL; // Invalid whence value
        }
    }
    
    // Update file position
    fobj->position = new_position;
    
    SYSCALL_TRACE("lseek(%i, %lli, %i) = %llu\n", handle, offset, whence, new_position);
    
    return static_cast<long>(new_position);
}

DECLARE_SYSCALL_HANDLER(writev) {
    /* -----------------------------------------------------------------
     *  Linux-style calling convention
     *      arg1 = int        fd
     *      arg2 = const struct iovec *  iov       (user pointer)
     *      arg3 = size_t     vlen
     *  We ignore arg4-arg5.
     * -----------------------------------------------------------------*/

    /* Minimal local definition; nothing needs to leak outside */
    struct iovec {
        void   *iov_base;   /* start of user buffer           */
        size_t  iov_len;    /* length of that buffer in bytes */
    };

    int handle = static_cast<int>(arg1);
    iovec* iov  = reinterpret_cast<iovec*>(arg2); /* user pointer */
    size_t vlen = arg3;

    // Validate parameters
    if (!iov) {
        return -EFAULT;
    }

    // Get handle entry from process handle table
    handle_entry* hentry = current->get_env()->handles.get_handle(handle);
    
    if (!hentry) {
        return -EBADF; // Bad file descriptor
    }

    size_t total_written = 0;

#ifdef STELLUX_STRACE_ENABLED
    kprint("writev(%d, [", handle);
#endif

    for (size_t i = 0; i < vlen; ++i) {
        /* Copy the iovec descriptor itself onto the kernel stack. */
        struct iovec k_iov;
        memcpy(&k_iov, &iov[i], sizeof k_iov);

#ifdef STELLUX_STRACE_ENABLED
        if (i > 0) kprint(", ");
        kprint("{iov_base=0x%llx, iov_len=%llu}", reinterpret_cast<uint64_t>(k_iov.iov_base), k_iov.iov_len);
#endif

        if (k_iov.iov_len == 0) {
            continue;
        }

        ssize_t n = 0;
        
        switch (hentry->type) {
            case handle_type::OUTPUT_STREAM: {
                // Write to serial port (stdout behavior)
                //serial::write(serial::g_kernel_uart_port, reinterpret_cast<char*>(k_iov.iov_base), k_iov.iov_len);
                char hack[512] = { 0 };
                memcpy(hack, k_iov.iov_base, k_iov.iov_len < 512 ? k_iov.iov_len : 511);
                kprint(hack);
                n = k_iov.iov_len;
                break;
            }
            
            case handle_type::FILE: {
                // Write to file
                fs::file_object* fobj = static_cast<fs::file_object*>(hentry->object);
                if (!fobj || !fobj->can_write()) {
                    return -EBADF;
                }
                
                // Use VFS write operation
                if (!fobj->vnode || !fobj->vnode->ops.write) {
                    return -EIO;
                }
                
                n = fobj->vnode->ops.write(fobj->vnode.get(), k_iov.iov_base, k_iov.iov_len, fobj->position);
                
                // Update file position on successful write
                if (n > 0) {
                    fobj->position += n;
                }
                break;
            }
            
            case handle_type::UNIX_SOCKET: {
                // Write to Unix domain socket
                auto socket_ptr = reinterpret_cast<kstl::shared_ptr<net::unix_stream_socket>*>(hentry->object);
                if (!socket_ptr || !(*socket_ptr)) {
                    return -EBADF;
                }
                
                auto socket = *socket_ptr;
                
                // Let the socket's write method handle state checking
                n = socket->write(k_iov.iov_base, k_iov.iov_len);
                break;
            }
            
            default:
                return -EBADF; // Unsupported handle type
        }

        // Propagate error as-is
        if (n < 0) {
            return n;
        }

        total_written += (size_t)n;

        if ((size_t)n < k_iov.iov_len) {
            break;   /* short write – stop early like Linux */
        }
    }

#ifdef STELLUX_STRACE_ENABLED
    kprint("], %llu) = %llu\n", vlen, total_written);
#endif

    /* On success return the total number of bytes consumed.  */
    return static_cast<long>(total_written);
}

DECLARE_SYSCALL_HANDLER(ioctl) {
    int fd = static_cast<int>(arg1);
    __unused fd;

    uint64_t req = arg2;
    void* userbuf = reinterpret_cast<void*>(arg3); 

    switch (req) {
    case 0x5413: { /* TIOCGWINSZ */
        struct winsize {
            unsigned short ws_row;
            unsigned short ws_col;
            unsigned short ws_xpixel;
            unsigned short ws_ypixel;
        };
        
        // Validate userbuf pointer before writing to it
        if (!userbuf) {
            return -EFAULT;
        }

        struct winsize ws = { 24, 80, 0, 0 };
        memcpy(userbuf, &ws, sizeof(struct winsize));

        SYSCALL_TRACE("ioctl(%d, TIOCGWINSZ, {ws_row=%d, ws_col=%d, ws_xpixel=%d, ws_ypixel=%d}) = 0\n",
               fd, ws.ws_row, ws.ws_col, ws.ws_xpixel, ws.ws_ypixel);
        return 0;
    }
    default:
        return -ENOTTY; /* "not a terminal" */
    }
} 