#include "syscall/handlers/sys_io.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "resource/socket_ops.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/heap.h"

using syscall::iovec;
using syscall::MAX_IOVCNT;

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
        case resource::ERR_IO:
        default:                      return syscall::EIO;
    }
}

DEFINE_SYSCALL3(readv, fd, iov_ptr, iovcnt) {
    if (iovcnt == 0) return 0;

    if (iovcnt > MAX_IOVCNT) return syscall::EINVAL;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    size_t iov_bytes = static_cast<size_t>(iovcnt) * sizeof(iovec);
    auto* kiovs = static_cast<iovec*>(heap::kzalloc(iov_bytes));
    if (!kiovs) return syscall::ENOMEM;

    int32_t copy_rc = mm::uaccess::copy_from_user(
        kiovs, reinterpret_cast<const iovec*>(iov_ptr), iov_bytes);
    if (copy_rc != mm::uaccess::OK) {
        heap::kfree(kiovs);
        return syscall::EFAULT;
    }

    int64_t total = 0;
    int64_t err = 0;
    size_t stage = syscall::io_chunk_size(task, static_cast<resource::handle_t>(fd));
    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(stage));
    if (!kbuf) {
        heap::kfree(kiovs);
        return syscall::ENOMEM;
    }

    bool done = false;
    for (uint64_t i = 0; i < iovcnt && !done; i++) {
        size_t remaining = kiovs[i].len;
        uint8_t* user_ptr = reinterpret_cast<uint8_t*>(kiovs[i].base);

        while (remaining > 0) {
            size_t chunk = remaining > stage ? stage : remaining;
            ssize_t n = resource::read(
                task, static_cast<resource::handle_t>(fd), kbuf, chunk);
            if (n < 0) {
                err = map_resource_error(n);
                done = true;
                break;
            }

            if (n == 0) {
                done = true;
                break;
            }

            copy_rc = mm::uaccess::copy_to_user(
                user_ptr, kbuf, static_cast<size_t>(n));
            if (copy_rc != mm::uaccess::OK) {
                err = syscall::EFAULT;
                done = true;
                break;
            }

            total += n;
            user_ptr += n;
            remaining -= static_cast<size_t>(n);
            if (static_cast<size_t>(n) < chunk) {
                done = true;
                break;
            }
        }
    }

    heap::kfree(kbuf);
    heap::kfree(kiovs);
    return total > 0 ? total : (err ? err : total);
}

DEFINE_SYSCALL3(writev, fd, iov_ptr, iovcnt) {
    if (iovcnt == 0) return 0;

    if (iovcnt > MAX_IOVCNT) return syscall::EINVAL;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    size_t iov_bytes = static_cast<size_t>(iovcnt) * sizeof(iovec);
    auto* kiovs = static_cast<iovec*>(heap::kzalloc(iov_bytes));
    if (!kiovs) return syscall::ENOMEM;

    int32_t copy_rc = mm::uaccess::copy_from_user(
        kiovs, reinterpret_cast<const iovec*>(iov_ptr), iov_bytes);
    if (copy_rc != mm::uaccess::OK) {
        heap::kfree(kiovs);
        return syscall::EFAULT;
    }

    int64_t total = 0;
    int64_t err = 0;
    size_t stage = syscall::io_chunk_size(task, static_cast<resource::handle_t>(fd));
    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(stage));
    if (!kbuf) {
        heap::kfree(kiovs);
        return syscall::ENOMEM;
    }

    bool done = false;
    for (uint64_t i = 0; i < iovcnt && !done; i++) {
        size_t remaining = kiovs[i].len;
        const uint8_t* user_ptr =
            reinterpret_cast<const uint8_t*>(kiovs[i].base);

        while (remaining > 0) {
            size_t chunk = remaining > stage ? stage : remaining;
            copy_rc = mm::uaccess::copy_from_user(kbuf, user_ptr, chunk);
            if (copy_rc != mm::uaccess::OK) {
                err = syscall::EFAULT;
                done = true;
                break;
            }

            ssize_t n = resource::write(
                task, static_cast<resource::handle_t>(fd), kbuf, chunk);
            if (n < 0) {
                err = map_resource_error(n);
                done = true;
                break;
            }

            if (n == 0) {
                done = true;
                break;
            }

            total += n;
            user_ptr += n;
            remaining -= static_cast<size_t>(n);
            if (static_cast<size_t>(n) < chunk) {
                done = true;
                break;
            }
        }
    }

    heap::kfree(kbuf);
    heap::kfree(kiovs);
    return total > 0 ? total : (err ? err : total);
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

    return map_resource_error(rc);
}
