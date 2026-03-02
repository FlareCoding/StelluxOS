#include "syscall/handlers/sys_net.h"
#include "syscall/resource_errno.h"

#include "mm/heap.h"
#include "mm/uaccess.h"
#include "net/unix_stream.h"
#include "resource/handle_table.h"
#include "resource/providers/socket_provider.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"

namespace {

constexpr uint64_t AF_UNIX = 1;
constexpr uint64_t SOCK_STREAM = 1;
constexpr uint64_t SOCK_TYPE_MASK = 0xF;
constexpr uint64_t SOCK_NONBLOCK = 0x800;
constexpr size_t IO_CHUNK_SIZE = 4096;

struct linux_sockaddr_un {
    uint16_t sun_family;
    char sun_path[net::unix_stream::SOCKET_PATH_MAX];
};

int64_t parse_socket_path(
    uint64_t user_addr,
    uint64_t user_len,
    net::unix_stream::socket_path* out_path
) {
    if (!out_path || user_addr == 0) {
        return syscall::EFAULT;
    }

    if (user_len < sizeof(uint16_t) || user_len > sizeof(linux_sockaddr_un)) {
        return syscall::EINVAL;
    }

    linux_sockaddr_un addr = {};
    int32_t copy_rc = mm::uaccess::copy_from_user(
        &addr,
        reinterpret_cast<const void*>(user_addr),
        static_cast<size_t>(user_len)
    );
    if (copy_rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }
    if (addr.sun_family != AF_UNIX) {
        return syscall::EAFNOSUPPORT;
    }

    size_t path_len = static_cast<size_t>(user_len) -
        __builtin_offsetof(linux_sockaddr_un, sun_path);
    int32_t path_rc = net::unix_stream::make_path(addr.sun_path, path_len, out_path);
    if (path_rc != net::unix_stream::OK) {
        return syscall::EINVAL;
    }
    return 0;
}

int64_t acquire_socket_resource(
    sched::task* task,
    resource::handle_t fd,
    resource::resource_object** out_obj
) {
    if (!task || !out_obj) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(&task->handles, fd, 0, &obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }
    if (obj->type != resource::resource_type::SOCKET) {
        resource::resource_release(obj);
        return syscall::ENOTSOCK;
    }

    *out_obj = obj;
    return 0;
}

} // anonymous namespace

DEFINE_SYSCALL3(socket, domain, type, protocol) {
    if (domain != AF_UNIX) {
        return syscall::EAFNOSUPPORT;
    }
    if (protocol != 0) {
        return syscall::EPROTONOSUPPORT;
    }

    uint64_t base_type = type & SOCK_TYPE_MASK;
    uint64_t extra_flags = type & ~SOCK_TYPE_MASK;
    if (base_type != SOCK_STREAM) {
        return syscall::EPROTONOSUPPORT;
    }
    if ((extra_flags & ~SOCK_NONBLOCK) != 0) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    bool nonblocking = (extra_flags & SOCK_NONBLOCK) != 0;
    resource::resource_object* obj = nullptr;
    int32_t rc = resource::socket_provider::create_stream_socket_resource(nonblocking, &obj);
    if (rc != resource::OK) {
        return syscall::map_resource_error(rc);
    }

    resource::handle_t handle = -1;
    rc = resource::alloc_handle(
        &task->handles,
        obj,
        resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE,
        &handle
    );
    resource::resource_release(obj);
    if (rc != resource::HANDLE_OK) {
        return (rc == resource::HANDLE_ERR_NOSPC) ? syscall::EMFILE : syscall::EIO;
    }

    return handle;
}

DEFINE_SYSCALL3(bind, fd, addr, addrlen) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    net::unix_stream::socket_path path = {};
    int64_t parse_rc = parse_socket_path(addr, addrlen, &path);
    if (parse_rc < 0) {
        return parse_rc;
    }

    resource::resource_object* obj = nullptr;
    int64_t acquire_rc = acquire_socket_resource(task, static_cast<resource::handle_t>(fd), &obj);
    if (acquire_rc < 0) {
        return acquire_rc;
    }

    int32_t rc = resource::socket_provider::bind(obj, path);
    resource::resource_release(obj);
    if (rc != resource::OK) {
        return syscall::map_resource_error(rc);
    }
    return 0;
}

DEFINE_SYSCALL2(listen, fd, backlog) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int64_t acquire_rc = acquire_socket_resource(task, static_cast<resource::handle_t>(fd), &obj);
    if (acquire_rc < 0) {
        return acquire_rc;
    }

    int32_t rc = resource::socket_provider::listen(obj, static_cast<uint32_t>(backlog));
    resource::resource_release(obj);
    if (rc != resource::OK) {
        return syscall::map_resource_error(rc);
    }
    return 0;
}

DEFINE_SYSCALL3(accept, fd, addr, addrlen) {
    if (addr != 0 || addrlen != 0) {
        return syscall::EOPNOTSUPP;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* listener = nullptr;
    int64_t acquire_rc = acquire_socket_resource(task, static_cast<resource::handle_t>(fd), &listener);
    if (acquire_rc < 0) {
        return acquire_rc;
    }

    resource::resource_object* accepted = nullptr;
    int32_t rc = resource::socket_provider::accept(listener, &accepted);
    resource::resource_release(listener);
    if (rc != resource::OK) {
        return syscall::map_resource_error(rc);
    }

    resource::handle_t new_fd = -1;
    rc = resource::alloc_handle(
        &task->handles,
        accepted,
        resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE,
        &new_fd
    );
    resource::resource_release(accepted);
    if (rc != resource::HANDLE_OK) {
        return (rc == resource::HANDLE_ERR_NOSPC) ? syscall::EMFILE : syscall::EIO;
    }

    return new_fd;
}

DEFINE_SYSCALL3(connect, fd, addr, addrlen) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    net::unix_stream::socket_path path = {};
    int64_t parse_rc = parse_socket_path(addr, addrlen, &path);
    if (parse_rc < 0) {
        return parse_rc;
    }

    resource::resource_object* obj = nullptr;
    int64_t acquire_rc = acquire_socket_resource(task, static_cast<resource::handle_t>(fd), &obj);
    if (acquire_rc < 0) {
        return acquire_rc;
    }

    int32_t rc = resource::socket_provider::connect(obj, path);
    resource::resource_release(obj);
    if (rc != resource::OK) {
        return syscall::map_resource_error(rc);
    }
    return 0;
}

DEFINE_SYSCALL6(sendto, fd, buf, len, flags, dest_addr, addrlen) {
    if (flags != 0) {
        return syscall::EOPNOTSUPP;
    }
    if (dest_addr != 0 || addrlen != 0) {
        return syscall::EOPNOTSUPP;
    }
    if (len == 0) {
        return 0;
    }
    if (buf == 0) {
        return syscall::EFAULT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int64_t acquire_rc = acquire_socket_resource(task, static_cast<resource::handle_t>(fd), &obj);
    if (acquire_rc < 0) {
        return acquire_rc;
    }

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(IO_CHUNK_SIZE));
    if (!kbuf) {
        resource::resource_release(obj);
        return syscall::ENOMEM;
    }

    const uint8_t* user_ptr = reinterpret_cast<const uint8_t*>(buf);
    size_t remaining = static_cast<size_t>(len);
    int64_t total = 0;

    while (remaining > 0) {
        size_t chunk = remaining > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : remaining;
        int32_t copy_rc = mm::uaccess::copy_from_user(kbuf, user_ptr, chunk);
        if (copy_rc != mm::uaccess::OK) {
            heap::kfree(kbuf);
            resource::resource_release(obj);
            if (total > 0) {
                return total;
            }
            return syscall::EFAULT;
        }

        if (!obj->ops || !obj->ops->write) {
            heap::kfree(kbuf);
            resource::resource_release(obj);
            if (total > 0) {
                return total;
            }
            return syscall::ENOSYS;
        }

        ssize_t n = obj->ops->write(obj, kbuf, chunk);
        if (n < 0) {
            heap::kfree(kbuf);
            resource::resource_release(obj);
            if (total > 0) {
                return total;
            }
            return syscall::map_resource_error(n);
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
    resource::resource_release(obj);
    return total;
}

DEFINE_SYSCALL6(recvfrom, fd, buf, len, flags, src_addr, addrlen) {
    if (flags != 0) {
        return syscall::EOPNOTSUPP;
    }
    if (src_addr != 0 || addrlen != 0) {
        return syscall::EOPNOTSUPP;
    }
    if (len == 0) {
        return 0;
    }
    if (buf == 0) {
        return syscall::EFAULT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int64_t acquire_rc = acquire_socket_resource(task, static_cast<resource::handle_t>(fd), &obj);
    if (acquire_rc < 0) {
        return acquire_rc;
    }

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(IO_CHUNK_SIZE));
    if (!kbuf) {
        resource::resource_release(obj);
        return syscall::ENOMEM;
    }

    uint8_t* user_ptr = reinterpret_cast<uint8_t*>(buf);
    size_t remaining = static_cast<size_t>(len);
    int64_t total = 0;

    while (remaining > 0) {
        size_t chunk = remaining > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : remaining;
        if (!obj->ops || !obj->ops->read) {
            heap::kfree(kbuf);
            resource::resource_release(obj);
            if (total > 0) {
                return total;
            }
            return syscall::ENOSYS;
        }

        ssize_t n = obj->ops->read(obj, kbuf, chunk);
        if (n < 0) {
            heap::kfree(kbuf);
            resource::resource_release(obj);
            if (total > 0) {
                return total;
            }
            return syscall::map_resource_error(n);
        }
        if (n == 0) {
            break;
        }

        int32_t copy_rc = mm::uaccess::copy_to_user(user_ptr, kbuf, static_cast<size_t>(n));
        if (copy_rc != mm::uaccess::OK) {
            heap::kfree(kbuf);
            resource::resource_release(obj);
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
    resource::resource_release(obj);
    return total;
}
