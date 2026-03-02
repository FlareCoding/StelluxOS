#include "syscall/handlers/sys_fd.h"

#include "resource/resource.h"
#include "resource/providers/socket_provider.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/heap.h"
#include "fs/fstypes.h"

namespace {

constexpr int64_t AT_FDCWD = -100;
constexpr size_t IO_CHUNK_SIZE = 4096;
constexpr uint64_t F_GETFL = 3;
constexpr uint64_t F_SETFL = 4;
constexpr uint64_t O_NONBLOCK = 0x800;

inline int64_t map_resource_error(int64_t rc) {
    switch (rc) {
        case resource::ERR_INVAL:
            return syscall::EINVAL;
        case resource::ERR_NOENT:
            return syscall::ENOENT;
        case resource::ERR_NOTDIR:
            return syscall::ENOTDIR;
        case resource::ERR_NAMETOOLONG:
            return syscall::ENAMETOOLONG;
        case resource::ERR_NOMEM:
            return syscall::ENOMEM;
        case resource::ERR_TABLEFULL:
            return syscall::EMFILE;
        case resource::ERR_AGAIN:
            return syscall::EAGAIN;
        case resource::ERR_PIPE:
            return syscall::EPIPE;
        case resource::ERR_ADDRINUSE:
            return syscall::EADDRINUSE;
        case resource::ERR_AFNOSUPPORT:
            return syscall::EAFNOSUPPORT;
        case resource::ERR_PROTONOSUPPORT:
            return syscall::EPROTONOSUPPORT;
        case resource::ERR_NOTCONN:
            return syscall::ENOTCONN;
        case resource::ERR_CONNREFUSED:
            return syscall::ECONNREFUSED;
        case resource::ERR_OPNOTSUPP:
            return syscall::EOPNOTSUPP;
        case resource::ERR_NOTSOCK:
            return syscall::ENOTSOCK;
        case resource::ERR_BADF:
        case resource::ERR_ACCESS:
            return syscall::EBADF;
        case resource::ERR_UNSUP:
            return syscall::ENOSYS;
        case resource::ERR_IO:
        default:
            return syscall::EIO;
    }
}

int64_t do_open_common(int64_t dirfd, uint64_t pathname, uint64_t flags, uint64_t mode) {
    (void)mode;

    if (dirfd != AT_FDCWD) {
        return syscall::EINVAL;
    }

    char kpath[fs::PATH_MAX];
    int32_t copy_rc = mm::uaccess::copy_cstr_from_user(
        kpath,
        sizeof(kpath),
        reinterpret_cast<const char*>(pathname)
    );
    if (copy_rc != mm::uaccess::OK) {
        if (copy_rc == mm::uaccess::ERR_NAMETOOLONG) {
            return syscall::ENAMETOOLONG;
        }
        return syscall::EFAULT;
    }

    if (kpath[0] != '/') {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::handle_t handle = -1;
    int32_t rc = resource::open(
        task,
        kpath,
        static_cast<uint32_t>(flags),
        &handle
    );
    if (rc != resource::OK) {
        return map_resource_error(rc);
    }

    return handle;
}

} // anonymous namespace

DEFINE_SYSCALL4(openat, dirfd, pathname, flags, mode) {
    return do_open_common(static_cast<int64_t>(dirfd), pathname, flags, mode);
}

DEFINE_SYSCALL3(open, pathname, flags, mode) {
    return do_open_common(AT_FDCWD, pathname, flags, mode);
}

DEFINE_SYSCALL3(read, fd, buf, count) {
    if (count == 0) {
        return 0;
    }
    if (buf == 0) {
        return syscall::EFAULT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    size_t remaining = static_cast<size_t>(count);
    uint8_t* user_ptr = reinterpret_cast<uint8_t*>(buf);
    int64_t total = 0;

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(IO_CHUNK_SIZE));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    while (remaining > 0) {
        size_t chunk = remaining > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : remaining;
        ssize_t n = resource::read(task, static_cast<resource::handle_t>(fd), kbuf, chunk);
        if (n < 0) {
            heap::kfree(kbuf);
            if (total > 0) {
                return total;
            }
            return map_resource_error(n);
        }
        if (n == 0) {
            break;
        }

        int32_t rc = mm::uaccess::copy_to_user(user_ptr, kbuf, static_cast<size_t>(n));
        if (rc != mm::uaccess::OK) {
            heap::kfree(kbuf);
            if (total > 0) {
                return total;
            }
            return syscall::EFAULT;
        }

        total += n;
        user_ptr += n;
        remaining -= static_cast<size_t>(n);

        if (static_cast<size_t>(n) < chunk) {
            break;
        }
    }

    heap::kfree(kbuf);
    return total;
}

DEFINE_SYSCALL3(write, fd, buf, count) {
    if (count == 0) {
        return 0;
    }
    if (buf == 0) {
        return syscall::EFAULT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    size_t remaining = static_cast<size_t>(count);
    const uint8_t* user_ptr = reinterpret_cast<const uint8_t*>(buf);
    int64_t total = 0;

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(IO_CHUNK_SIZE));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    while (remaining > 0) {
        size_t chunk = remaining > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : remaining;
        int32_t copy_rc = mm::uaccess::copy_from_user(kbuf, user_ptr, chunk);
        if (copy_rc != mm::uaccess::OK) {
            heap::kfree(kbuf);
            if (total > 0) {
                return total;
            }
            return syscall::EFAULT;
        }

        ssize_t n = resource::write(task, static_cast<resource::handle_t>(fd), kbuf, chunk);
        if (n < 0) {
            heap::kfree(kbuf);
            if (total > 0) {
                return total;
            }
            return map_resource_error(n);
        }
        if (n == 0) {
            break;
        }

        total += n;
        user_ptr += n;
        remaining -= static_cast<size_t>(n);

        if (static_cast<size_t>(n) < chunk) {
            break;
        }
    }

    heap::kfree(kbuf);
    return total;
}

DEFINE_SYSCALL1(close, fd) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    int32_t rc = resource::close(task, static_cast<resource::handle_t>(fd));
    if (rc != resource::OK) {
        return map_resource_error(rc);
    }
    return 0;
}

DEFINE_SYSCALL3(fcntl, fd, cmd, arg) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles,
        static_cast<resource::handle_t>(fd),
        0,
        &obj
    );
    if (rc != resource::HANDLE_OK) {
        return map_resource_error(resource::ERR_BADF);
    }

    if (cmd == F_GETFL) {
        if (obj->type != resource::resource_type::SOCKET) {
            resource::resource_release(obj);
            return 0;
        }

        bool nonblocking = false;
        rc = resource::socket_provider::get_nonblocking(obj, &nonblocking);
        resource::resource_release(obj);
        if (rc != resource::OK) {
            return map_resource_error(rc);
        }

        return nonblocking ? static_cast<int64_t>(O_NONBLOCK) : 0;
    }

    if (cmd == F_SETFL) {
        if (obj->type != resource::resource_type::SOCKET) {
            resource::resource_release(obj);
            return 0;
        }
        if ((arg & ~O_NONBLOCK) != 0) {
            resource::resource_release(obj);
            return syscall::EINVAL;
        }

        bool nonblocking = (arg & O_NONBLOCK) != 0;
        rc = resource::socket_provider::set_nonblocking(obj, nonblocking);
        resource::resource_release(obj);
        if (rc != resource::OK) {
            return map_resource_error(rc);
        }
        return 0;
    }

    resource::resource_release(obj);
    return syscall::EINVAL;
}
