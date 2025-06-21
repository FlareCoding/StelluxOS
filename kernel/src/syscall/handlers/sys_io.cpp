#include <syscall/handlers/sys_io.h>
#include <serial/serial.h>
#include <fs/vfs.h>
#include <process/process.h>
#include <core/klog.h>

DECLARE_SYSCALL_HANDLER(write) {
    int fd = static_cast<int>(arg1);
    __unused fd;

    const char* buf = reinterpret_cast<const char*>(arg2);
    size_t count = static_cast<size_t>(arg3);
    
    // Write to serial port
    serial::write(serial::g_kernel_uart_port, buf, count);
    
    SYSCALL_TRACE("write(%i, \"0x%llx\", %llu) = %llu\n", fd, reinterpret_cast<uint64_t>(buf), count, count);
    return static_cast<long>(count);
}

DECLARE_SYSCALL_HANDLER(read) {
    // Handle read syscall
    // TODO: Implement file reading
    return -ENOSYS;
}

DECLARE_SYSCALL_HANDLER(open) {
    // Handle open syscall
    // TODO: Implement file opening
    return -ENOSYS;
}

DECLARE_SYSCALL_HANDLER(close) {
    // Handle close syscall
    // TODO: Implement file closing
    return -ENOSYS;
}

DECLARE_SYSCALL_HANDLER(lseek) {
    // Handle lseek syscall
    // TODO: Implement file seeking
    return -ENOSYS;
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

    int fd = static_cast<int>(arg1);
    __unused fd;

    iovec* iov  = reinterpret_cast<iovec*>(arg2); /* user pointer */
    size_t vlen = arg3;

    size_t total_written = 0;

#ifdef STELLUX_STRACE_ENABLED
    kprint("writev(%llu, [", fd);
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

        ssize_t n = k_iov.iov_len;
        serial::write(serial::g_kernel_uart_port, reinterpret_cast<char*>(k_iov.iov_base), n);

        if (n < 0) {                 /* propagate error as-is */
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
    if (total_written > 0) {
        return (ssize_t)total_written;
    }
    return 0;
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