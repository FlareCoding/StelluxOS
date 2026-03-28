#include "syscall/handlers/sys_socket.h"

#include "socket/unix_socket.h"
#include "net/inet_socket.h"
#include "net/tcp.h"
#include "resource/resource.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/heap.h"

namespace {

constexpr uint64_t AF_UNIX     = 1;
constexpr uint64_t AF_INET     = 2;
constexpr uint64_t SOCK_STREAM = 1;
constexpr uint64_t SOCK_DGRAM  = 2;
constexpr uint64_t IPPROTO_ICMP = 1;
constexpr uint64_t IPPROTO_TCP  = 6;
constexpr uint64_t IPPROTO_UDP  = 17;
constexpr size_t   SENDTO_MAX_ADDR = 128;
constexpr size_t   SENDTO_MAX_BUF  = 4096;

inline int64_t map_socket_op_error(int32_t rc) {
    switch (rc) {
    case resource::ERR_INVAL:       return syscall::EINVAL;
    case resource::ERR_NOMEM:       return syscall::ENOMEM;
    case resource::ERR_ADDRINUSE:   return syscall::EADDRINUSE;
    case resource::ERR_CONNREFUSED: return syscall::ECONNREFUSED;
    case resource::ERR_ISCONN:      return syscall::EISCONN;
    case resource::ERR_AGAIN:       return syscall::EAGAIN;
    case resource::ERR_NOENT:       return syscall::ENOENT;
    case resource::ERR_NOTDIR:      return syscall::ENOTDIR;
    case resource::ERR_INTR:        return syscall::EINTR;
    default:                        return syscall::EIO;
    }
}

} // anonymous namespace

DEFINE_SYSCALL3(socket, domain, type, protocol) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc;

    if (domain == AF_UNIX) {
        if (type != SOCK_STREAM || protocol != 0) {
            return syscall::EINVAL;
        }
        rc = socket::create_unbound_socket(&obj);
    } else if (domain == AF_INET) {
        if (type == SOCK_STREAM && (protocol == 0 || protocol == IPPROTO_TCP)) {
            rc = net::create_tcp_socket(&obj);
        } else if (type == SOCK_DGRAM && protocol == IPPROTO_ICMP) {
            rc = net::create_inet_icmp_socket(&obj);
        } else if (type == SOCK_DGRAM && (protocol == 0 || protocol == IPPROTO_UDP)) {
            rc = net::create_inet_udp_socket(&obj);
        } else {
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
        &task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h
    );
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(obj);
        return syscall::EMFILE;
    }
    resource::resource_release(obj);
    return h;
}

DEFINE_SYSCALL4(socketpair, domain, type, protocol, sv) {
    if (domain != AF_UNIX) {
        return syscall::EINVAL;
    }
    if (type != SOCK_STREAM) {
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
        &task->handles, obj_a, resource::resource_type::SOCKET,
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
        &task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1
    );
    if (rc != resource::HANDLE_OK) {
        resource::close(task, h0);
        resource::resource_release(obj_b);
        return syscall::EMFILE;
    }
    resource::resource_release(obj_b);

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
        &task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;
    if (obj->type != resource::resource_type::SOCKET || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }
    if (!obj->ops || !obj->ops->bind) {
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

    int32_t result = obj->ops->bind(obj, kaddr, klen);
    resource::resource_release(obj);
    return (result == resource::OK) ? 0 : map_socket_op_error(result);
}

DEFINE_SYSCALL2(listen, fd, backlog) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;
    if (obj->type != resource::resource_type::SOCKET || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }
    if (!obj->ops || !obj->ops->listen) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
    }

    int32_t result = obj->ops->listen(obj, static_cast<int32_t>(backlog));
    resource::resource_release(obj);
    return (result == resource::OK) ? 0 : map_socket_op_error(result);
}

DEFINE_SYSCALL3(connect, fd, addr, addrlen) {
    if (addr == 0) return syscall::EFAULT;
    if (addrlen < sizeof(uint16_t)) return syscall::EINVAL;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;
    if (obj->type != resource::resource_type::SOCKET || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }
    if (!obj->ops || !obj->ops->connect) {
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

    int32_t result = obj->ops->connect(obj, kaddr, klen);
    resource::resource_release(obj);
    return (result == resource::OK) ? 0 : map_socket_op_error(result);
}

DEFINE_SYSCALL3(accept, fd, addr, addrlen) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* listen_obj = nullptr;
    uint32_t handle_flags = 0;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &listen_obj, &handle_flags);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;
    if (listen_obj->type != resource::resource_type::SOCKET || !listen_obj->impl) {
        resource::resource_release(listen_obj);
        return syscall::EINVAL;
    }
    if (!listen_obj->ops || !listen_obj->ops->accept) {
        resource::resource_release(listen_obj);
        return syscall::EOPNOTSUPP;
    }

    bool nonblock = (handle_flags & fs::O_NONBLOCK) != 0;

    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t kaddr_len = sizeof(kaddr);

    resource::resource_object* new_obj = nullptr;
    int32_t result = listen_obj->ops->accept(
        listen_obj, &new_obj, kaddr, &kaddr_len, nonblock);

    if (result != resource::OK) {
        resource::resource_release(listen_obj);
        return map_socket_op_error(result);
    }

    resource::handle_t new_handle = -1;
    rc = resource::alloc_handle(
        &task->handles, new_obj, resource::resource_type::SOCKET,
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
        &task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_WRITE, &obj
    );
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }
    if (!obj->ops || !obj->ops->sendto) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
    }

    // Copy data from userspace
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

    // Copy sockaddr from userspace
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

    ssize_t result = obj->ops->sendto(obj, kbuf, data_len,
                                       static_cast<uint32_t>(flags),
                                       kaddr, addr_len);
    heap::kfree(kbuf);
    resource::resource_release(obj);

    if (result < 0) {
        return syscall::EIO;
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
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &obj
    );
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }
    if (!obj->ops || !obj->ops->recvfrom) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
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

    ssize_t result = obj->ops->recvfrom(obj, kbuf, data_len,
                                         static_cast<uint32_t>(flags),
                                         kaddr, &kaddr_len);

    if (result < 0) {
        heap::kfree(kbuf);
        resource::resource_release(obj);
        if (result == resource::ERR_AGAIN) {
            return syscall::EAGAIN;
        }
        return syscall::EIO;
    }

    // Copy data to userspace
    int32_t copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(buf), kbuf, static_cast<size_t>(result));
    heap::kfree(kbuf);
    if (copy_rc != mm::uaccess::OK) {
        resource::resource_release(obj);
        return syscall::EFAULT;
    }

    // Copy source address to userspace if requested
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
