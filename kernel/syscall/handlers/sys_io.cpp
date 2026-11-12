#include "syscall/handlers/sys_io.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "resource/socket_ops.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/heap.h"

using syscall::iovec;

__PRIVILEGED_CODE size_t syscall::io_chunk_size(sched::task* task, resource::handle_t fd) {
    resource::resource_object* obj = nullptr;
    if (resource::get_handle_object(task->handles, fd, 0, &obj) != resource::HANDLE_OK) {
        return IO_CHUNK_SIZE;
    }

    const resource::socket_ops* ops = resource::socket_ops_of(obj);
    size_t size = ops && ops->stream ? STREAM_CHUNK_SIZE : IO_CHUNK_SIZE;
    resource::resource_release(obj);

    return size;
}

static inline int64_t map_resource_error(int64_t rc) {
    switch (rc) {
        case resource::ERR_INVAL:     return syscall::EINVAL;
        case resource::ERR_NOENT:     return syscall::ENOENT;
        case resource::ERR_BADF:
        case resource::ERR_ACCESS:    return syscall::EBADF;
        case resource::ERR_NOMEM:     return syscall::ENOMEM;
        case resource::ERR_TABLEFULL: return syscall::EMFILE;
        case resource::ERR_UNSUP:     return syscall::ENOSYS;
        case resource::ERR_PIPE:      return syscall::EPIPE;
        case resource::ERR_INTR:      return syscall::ERESTARTSYS;
        case resource::ERR_AGAIN:     return syscall::EAGAIN;
        case resource::ERR_BUSY:      return syscall::EBUSY;
        case resource::ERR_NODATA:    return syscall::ENODATA;
        case resource::ERR_IO:
        default:                      return syscall::EIO;
    }
}

// Adds up the lengths of the buffers `iovs` describe, failing when the total cannot be reported
static bool total_length(const iovec* iovs, uint64_t iovcnt, size_t* out_total) {
    size_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (__builtin_add_overflow(total, iovs[i].len, &total) || static_cast<int64_t>(total) < 0) {
            return false;
        }
    }

    *out_total = total;
    return true;
}

__PRIVILEGED_CODE int64_t syscall::gather_from_user(const iovec* iovs, uint64_t iovcnt, uint8_t* data) {
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (iovs[i].len == 0) {
            continue;
        }

        const void* source = reinterpret_cast<const void*>(iovs[i].base);
        if (mm::uaccess::copy_from_user(data, source, iovs[i].len) != mm::uaccess::OK) {
            return syscall::EFAULT;
        }

        data += iovs[i].len;
    }

    return 0;
}

__PRIVILEGED_CODE int64_t syscall::scatter_to_user(const iovec* iovs, uint64_t iovcnt,
                                                   const uint8_t* data, size_t len) {
    for (uint64_t i = 0; i < iovcnt && len > 0; i++) {
        size_t chunk = iovs[i].len < len ? iovs[i].len : len;
        if (chunk == 0) {
            continue;
        }

        if (mm::uaccess::copy_to_user(reinterpret_cast<void*>(iovs[i].base), data, chunk) != mm::uaccess::OK) {
            return syscall::EFAULT;
        }

        data += chunk;
        len -= chunk;
    }

    return 0;
}

__PRIVILEGED_CODE size_t syscall::message_limit(sched::task* task, resource::handle_t fd) {
    resource::resource_object* obj = nullptr;
    if (resource::get_handle_object(task->handles, fd, 0, &obj) != resource::HANDLE_OK) {
        return 0;
    }

    const resource::socket_ops* ops = resource::socket_ops_of(obj);
    size_t max_message = ops && !ops->stream ? ops->max_message : 0;
    resource::resource_release(obj);

    return max_message;
}

__PRIVILEGED_CODE int64_t syscall::read_message(sched::task* task, resource::handle_t fd, const iovec* iovs,
                                                uint64_t iovcnt, size_t max_message, uint32_t call_flags) {
    size_t total = 0;
    if (!total_length(iovs, iovcnt, &total)) {
        return syscall::EINVAL;
    }

    // A read with no room takes no message
    if (total == 0) {
        return 0;
    }

    size_t staged = total < max_message ? total : max_message;
    auto* kbuf = static_cast<uint8_t*>(heap::uzalloc(staged));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    ssize_t n = resource::read(task, fd, kbuf, staged, call_flags);
    int64_t result = n < 0 ? map_resource_error(n) : n;
    if (n > 0 && scatter_to_user(iovs, iovcnt, kbuf, static_cast<size_t>(n)) != 0) {
        result = syscall::EFAULT;
    }

    heap::ufree(kbuf);

    return result;
}

__PRIVILEGED_CODE int64_t syscall::write_message(sched::task* task, resource::handle_t fd, const iovec* iovs,
                                                 uint64_t iovcnt, size_t max_message, uint32_t call_flags) {
    size_t total = 0;
    if (!total_length(iovs, iovcnt, &total)) {
        return syscall::EINVAL;
    }

    if (total == 0) {
        return 0;
    }

    if (total > max_message) {
        return syscall::EMSGSIZE;
    }

    auto* kbuf = static_cast<uint8_t*>(heap::uzalloc(total));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    int64_t result = gather_from_user(iovs, iovcnt, kbuf);
    if (result == 0) {
        ssize_t n = resource::write(task, fd, kbuf, total, call_flags);
        result = n < 0 ? map_resource_error(n) : n;
    }

    heap::ufree(kbuf);

    return result;
}

DEFINE_SYSCALL3(ioctl, fd, cmd, arg) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    int32_t rc = resource::ioctl(
        task, static_cast<resource::handle_t>(fd),
        static_cast<uint32_t>(cmd), arg);
    if (rc == resource::OK) return 0;

    // POSIX: an unsupported request on any fd is ENOTTY, not ENOSYS
    if (rc == resource::ERR_UNSUP) return syscall::ENOTTY;

    // Here, not in map_resource_error, whose read and write callers report a timed-out TCP connection as EIO
    if (rc == resource::ERR_TIMEDOUT) return syscall::ETIMEDOUT;

    return map_resource_error(rc);
}
