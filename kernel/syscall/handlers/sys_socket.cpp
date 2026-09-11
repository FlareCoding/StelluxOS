#include "syscall/handlers/sys_socket.h"
#include "syscall/handlers/sys_error_map.h"

#include "socket/unix_socket.h"
#include "net/inet.h"
#include "resource/socket_ops.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/heap.h"

constexpr uint64_t AF_UNIX     = 1;
constexpr uint64_t SOCK_STREAM = 1;
constexpr size_t   SENDTO_MAX_ADDR = 128;
constexpr size_t   SENDTO_MAX_BUF  = 4096;

constexpr uint64_t SOCK_CREATION_FLAGS = fs::O_NONBLOCK | fs::O_CLOEXEC;

__PRIVILEGED_CODE static void apply_creation_flags(sched::task* task, resource::handle_t h,
                                                   uint64_t creation_flags) {
    uint32_t flags = (creation_flags & fs::O_NONBLOCK) ? fs::O_NONBLOCK : 0;
    if (creation_flags & fs::O_CLOEXEC) {
        flags |= resource::RESOURCE_HANDLE_CLOEXEC;
    }

    if (flags) {
        resource::set_handle_flags(task->handles, h, flags);
    }
}

DEFINE_SYSCALL3(socket, domain, type, protocol) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    uint64_t creation_flags = type & SOCK_CREATION_FLAGS;
    type &= ~SOCK_CREATION_FLAGS;

    resource::resource_object* obj = nullptr;
    int32_t rc;

    if (domain == AF_UNIX) {
        if (type != SOCK_STREAM || protocol != 0) {
            return syscall::EINVAL;
        }

        rc = socket::create_unbound_socket(&obj);
    } else if (domain == net::inet::AF_INET) {
        rc = net::inet::create_socket(static_cast<uint32_t>(type),
                                      static_cast<uint32_t>(protocol), &obj);

        if (rc == resource::ERR_UNSUP) {
            return syscall::EPROTONOSUPPORT;
        }
    } else {
        return syscall::EAFNOSUPPORT;
    }

    if (rc != resource::OK) {
        return syscall::ENOMEM;
    }

    resource::handle_t h = -1;
    rc = resource::alloc_handle(
        task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h
    );
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(obj);
        return syscall::EMFILE;
    }

    apply_creation_flags(task, h, creation_flags);
    resource::resource_release(obj);
    return h;
}

DEFINE_SYSCALL4(socketpair, domain, type, protocol, sv) {
    if (domain != AF_UNIX) {
        return syscall::EINVAL;
    }

    uint64_t creation_flags = type & SOCK_CREATION_FLAGS;
    if ((type & ~SOCK_CREATION_FLAGS) != SOCK_STREAM) {
        return syscall::EINVAL;
    }

    if (protocol != 0) {
        return syscall::EINVAL;
    }

    if (sv == 0) {
        return syscall::EFAULT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    int32_t rc = socket::create_socket_pair(&obj_a, &obj_b);
    if (rc != resource::OK) {
        return syscall::ENOMEM;
    }

    resource::handle_t h0 = -1;
    rc = resource::alloc_handle(
        task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0
    );
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(obj_a);
        resource::resource_release(obj_b);
        return syscall::EMFILE;
    }

    resource::resource_release(obj_a);

    resource::handle_t h1 = -1;
    rc = resource::alloc_handle(
        task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1
    );
    if (rc != resource::HANDLE_OK) {
        resource::close(task, h0);
        resource::resource_release(obj_b);
        return syscall::EMFILE;
    }

    resource::resource_release(obj_b);

    apply_creation_flags(task, h0, creation_flags);
    apply_creation_flags(task, h1, creation_flags);

    int32_t kbuf[2] = {h0, h1};
    int32_t copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(sv), kbuf, sizeof(kbuf)
    );
    if (copy_rc != mm::uaccess::OK) {
        resource::close(task, h1);
        resource::close(task, h0);
        return syscall::EFAULT;
    }

    return 0;
}

DEFINE_SYSCALL3(bind, fd, addr, addrlen) {
    if (addr == 0) return syscall::EFAULT;

    if (addrlen < sizeof(uint16_t)) return syscall::EINVAL;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;

    if (obj->type != resource::resource_type::SOCKET || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    const resource::socket_ops* sockops = resource::socket_ops_of(obj);
    if (!sockops || !sockops->bind) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
    }

    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t klen = static_cast<size_t>(addrlen);
    if (klen > SENDTO_MAX_ADDR) klen = SENDTO_MAX_ADDR;
    int32_t copy_rc = mm::uaccess::copy_from_user(
        kaddr, reinterpret_cast<const void*>(addr), klen);
    if (copy_rc != mm::uaccess::OK) {
        resource::resource_release(obj);
        return syscall::EFAULT;
    }

    int32_t result = sockops->bind(obj, kaddr, klen);
    resource::resource_release(obj);
    return (result == resource::OK) ? 0 : syscall::error_map::map_socket_op_error(result);
}

DEFINE_SYSCALL2(listen, fd, backlog) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;

    if (obj->type != resource::resource_type::SOCKET || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    const resource::socket_ops* sockops = resource::socket_ops_of(obj);
    if (!sockops || !sockops->listen) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
    }

    int32_t result = sockops->listen(obj, static_cast<int32_t>(backlog));
    resource::resource_release(obj);
    return (result == resource::OK) ? 0 : syscall::error_map::map_socket_op_error(result);
}

DEFINE_SYSCALL3(connect, fd, addr, addrlen) {
    if (addr == 0) return syscall::EFAULT;

    if (addrlen < sizeof(uint16_t)) return syscall::EINVAL;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;

    if (obj->type != resource::resource_type::SOCKET || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    const resource::socket_ops* sockops = resource::socket_ops_of(obj);
    if (!sockops || !sockops->connect) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
    }

    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t klen = static_cast<size_t>(addrlen);
    if (klen > SENDTO_MAX_ADDR) klen = SENDTO_MAX_ADDR;
    int32_t copy_rc = mm::uaccess::copy_from_user(
        kaddr, reinterpret_cast<const void*>(addr), klen);
    if (copy_rc != mm::uaccess::OK) {
        resource::resource_release(obj);
        return syscall::EFAULT;
    }

    int32_t result = sockops->connect(obj, kaddr, klen);
    resource::resource_release(obj);
    return (result == resource::OK) ? 0 : syscall::error_map::map_socket_op_error(result);
}

DEFINE_SYSCALL3(accept, fd, addr, addrlen) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* listen_obj = nullptr;
    uint32_t handle_flags = 0;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &listen_obj, &handle_flags);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;

    if (listen_obj->type != resource::resource_type::SOCKET || !listen_obj->impl) {
        resource::resource_release(listen_obj);
        return syscall::EINVAL;
    }

    const resource::socket_ops* sockops = resource::socket_ops_of(listen_obj);
    if (!sockops || !sockops->accept) {
        resource::resource_release(listen_obj);
        return syscall::EOPNOTSUPP;
    }

    bool nonblock = (handle_flags & fs::O_NONBLOCK) != 0;

    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t kaddr_len = sizeof(kaddr);

    resource::resource_object* new_obj = nullptr;
    int32_t result = sockops->accept(
        listen_obj, &new_obj, kaddr, &kaddr_len, nonblock);

    if (result != resource::OK) {
        resource::resource_release(listen_obj);
        return syscall::error_map::map_socket_op_error(result);
    }

    resource::handle_t new_handle = -1;
    rc = resource::alloc_handle(
        task->handles, new_obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &new_handle);
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(new_obj);
        resource::resource_release(listen_obj);
        return syscall::EMFILE;
    }

    resource::resource_release(new_obj);
    resource::resource_release(listen_obj);

    if (addr != 0 && addrlen != 0) {
        uint32_t user_addrlen = 0;
        int32_t copy_rc = mm::uaccess::copy_from_user(
            &user_addrlen, reinterpret_cast<const void*>(addrlen),
            sizeof(user_addrlen));
        if (copy_rc == mm::uaccess::OK) {
            size_t copy_len = kaddr_len < user_addrlen ? kaddr_len : user_addrlen;
            if (copy_len > 0) {
                mm::uaccess::copy_to_user(
                    reinterpret_cast<void*>(addr), kaddr, copy_len);
            }
            uint32_t out_len = static_cast<uint32_t>(kaddr_len);
            mm::uaccess::copy_to_user(
                reinterpret_cast<void*>(addrlen), &out_len, sizeof(out_len));
        }
    }

    return new_handle;
}

DEFINE_SYSCALL6(sendto, fd, buf, len, flags, dest_addr, addrlen) {
    if (buf == 0 || len == 0) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_WRITE, &obj
    );
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    const resource::socket_ops* sockops = resource::socket_ops_of(obj);
    if (!sockops || !sockops->sendto) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
    }

    size_t data_len = static_cast<size_t>(len);
    if (data_len > SENDTO_MAX_BUF) {
        resource::resource_release(obj);
        return syscall::EMSGSIZE;
    }

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(data_len));
    if (!kbuf) {
        resource::resource_release(obj);
        return syscall::ENOMEM;
    }

    int32_t copy_rc = mm::uaccess::copy_from_user(kbuf, reinterpret_cast<const void*>(buf), data_len);
    if (copy_rc != mm::uaccess::OK) {
        heap::kfree(kbuf);
        resource::resource_release(obj);
        return syscall::EFAULT;
    }

    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t addr_len = 0;
    if (dest_addr != 0 && addrlen > 0) {
        addr_len = static_cast<size_t>(addrlen);
        if (addr_len > SENDTO_MAX_ADDR) {
            heap::kfree(kbuf);
            resource::resource_release(obj);
            return syscall::EINVAL;
        }

        copy_rc = mm::uaccess::copy_from_user(kaddr, reinterpret_cast<const void*>(dest_addr), addr_len);
        if (copy_rc != mm::uaccess::OK) {
            heap::kfree(kbuf);
            resource::resource_release(obj);
            return syscall::EFAULT;
        }
    }

    ssize_t result = sockops->sendto(obj, kbuf, data_len,
                                  static_cast<uint32_t>(flags),
                                  kaddr, addr_len);
    heap::kfree(kbuf);
    resource::resource_release(obj);

    if (result < 0) {
        return syscall::error_map::map_socket_op_error(static_cast<int32_t>(result));
    }

    return result;
}

DEFINE_SYSCALL6(recvfrom, fd, buf, len, flags, src_addr, addrlen) {
    if (buf == 0 || len == 0) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    uint32_t handle_flags = 0;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj, &handle_flags
    );
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    const resource::socket_ops* sockops = resource::socket_ops_of(obj);
    if (!sockops || !sockops->recvfrom) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
    }

    // A nonblocking descriptor never waits, whatever the call asked for
    uint32_t msg_flags = static_cast<uint32_t>(flags);
    if (handle_flags & fs::O_NONBLOCK) {
        msg_flags |= net::inet::MSG_DONTWAIT;
    }

    size_t data_len = static_cast<size_t>(len);
    if (data_len > SENDTO_MAX_BUF) {
        data_len = SENDTO_MAX_BUF;
    }

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(data_len));
    if (!kbuf) {
        resource::resource_release(obj);
        return syscall::ENOMEM;
    }

    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t kaddr_len = sizeof(kaddr);

    ssize_t result = sockops->recvfrom(obj, kbuf, data_len, msg_flags,
                                    kaddr, &kaddr_len);

    if (result < 0) {
        heap::kfree(kbuf);
        resource::resource_release(obj);
        return syscall::error_map::map_socket_op_error(static_cast<int32_t>(result));
    }

    int32_t copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(buf), kbuf, static_cast<size_t>(result));
    heap::kfree(kbuf);
    if (copy_rc != mm::uaccess::OK) {
        resource::resource_release(obj);
        return syscall::EFAULT;
    }

    if (src_addr != 0 && addrlen != 0) {
        uint32_t user_addrlen = 0;
        copy_rc = mm::uaccess::copy_from_user(
            &user_addrlen, reinterpret_cast<const void*>(addrlen), sizeof(user_addrlen));
        if (copy_rc != mm::uaccess::OK) {
            resource::resource_release(obj);
            return syscall::EFAULT;
        }

        size_t copy_addr_len = kaddr_len < user_addrlen ? kaddr_len : user_addrlen;
        if (copy_addr_len > 0) {
            copy_rc = mm::uaccess::copy_to_user(
                reinterpret_cast<void*>(src_addr), kaddr, copy_addr_len);
            if (copy_rc != mm::uaccess::OK) {
                resource::resource_release(obj);
                return syscall::EFAULT;
            }
        }

        uint32_t out_len = static_cast<uint32_t>(kaddr_len);
        copy_rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(addrlen), &out_len, sizeof(out_len));
        if (copy_rc != mm::uaccess::OK) {
            resource::resource_release(obj);
            return syscall::EFAULT;
        }
    }

    resource::resource_release(obj);
    return result;
}

DEFINE_SYSCALL5(setsockopt, fd, level, optname, optval, optlen) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;

    if (obj->type != resource::resource_type::SOCKET || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    const resource::socket_ops* sockops = resource::socket_ops_of(obj);
    if (!sockops || !sockops->setsockopt) {
        resource::resource_release(obj);
        return syscall::ENOPROTOOPT;
    }

    size_t klen = static_cast<size_t>(optlen);
    if (klen == 0 || klen > 64) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    uint8_t kval[64] = {};
    int32_t copy_rc = mm::uaccess::copy_from_user(
        kval, reinterpret_cast<const void*>(optval), klen);
    if (copy_rc != mm::uaccess::OK) {
        resource::resource_release(obj);
        return syscall::EFAULT;
    }

    int32_t result = sockops->setsockopt(
        obj, static_cast<int32_t>(level), static_cast<int32_t>(optname),
        kval, klen);
    resource::resource_release(obj);
    return (result == resource::OK) ? 0 : syscall::error_map::map_socket_op_error(result);
}

DEFINE_SYSCALL5(getsockopt, fd, level, optname, optval, optlen) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    if (obj->type != resource::resource_type::SOCKET || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    const resource::socket_ops* sockops = resource::socket_ops_of(obj);
    if (!sockops || !sockops->getsockopt) {
        resource::resource_release(obj);
        return syscall::ENOPROTOOPT;
    }

    uint32_t user_len = 0;
    int32_t copy_rc = mm::uaccess::copy_from_user(
        &user_len, reinterpret_cast<const void*>(optlen), sizeof(user_len));
    if (copy_rc != mm::uaccess::OK) {
        resource::resource_release(obj);
        return syscall::EFAULT;
    }

    size_t klen = user_len;
    if (klen == 0 || klen > 64) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    uint8_t kval[64] = {};
    int32_t result = sockops->getsockopt(
        obj, static_cast<int32_t>(level), static_cast<int32_t>(optname),
        kval, &klen);
    if (result != resource::OK) {
        resource::resource_release(obj);
        return syscall::error_map::map_socket_op_error(result);
    }

    copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(optval), kval, klen);
    if (copy_rc != mm::uaccess::OK) {
        resource::resource_release(obj);
        return syscall::EFAULT;
    }

    uint32_t out_len = static_cast<uint32_t>(klen);
    copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(optlen), &out_len, sizeof(out_len));
    if (copy_rc != mm::uaccess::OK) {
        resource::resource_release(obj);
        return syscall::EFAULT;
    }

    resource::resource_release(obj);
    return 0;
}
