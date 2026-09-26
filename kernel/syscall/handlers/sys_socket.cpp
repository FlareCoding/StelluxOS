#include "syscall/handlers/sys_socket.h"
#include "syscall/handlers/sys_error_map.h"
#include "syscall/handlers/sys_io.h"

#include "socket/unix_socket.h"
#include "net/inet.h"
#include "resource/socket_ops.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/poll.h"
#include "mm/uaccess.h"
#include "mm/heap.h"
#include "common/string.h"

constexpr uint64_t AF_UNIX     = 1;
constexpr uint64_t SOCK_STREAM = 1;
constexpr size_t   SENDTO_MAX_ADDR = 128;
constexpr size_t   SENDTO_MAX_BUF  = 4096;

constexpr uint64_t SOCK_CREATION_FLAGS = fs::O_NONBLOCK | fs::O_CLOEXEC;

// The message header as userland lays it out on both 64-bit targets
struct msghdr {
    uint64_t name;
    uint32_t namelen;
    uint32_t pad0;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control;
    uint64_t controllen;
    uint32_t flags;
    uint32_t pad1;
};
static_assert(sizeof(msghdr) == 56);

struct socket_ref {
    resource::resource_object* obj = nullptr;
    const resource::socket_ops* ops = nullptr;
    uint32_t status_flags = 0;
};

__PRIVILEGED_CODE static void apply_cloexec(sched::task* task, resource::handle_t h, uint64_t creation_flags) {
    if (creation_flags & fs::O_CLOEXEC) {
        resource::set_handle_flags(task->handles, h, resource::RESOURCE_HANDLE_CLOEXEC);
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

    resource::set_status_flags(obj, static_cast<uint32_t>(creation_flags));

    resource::handle_t h = -1;
    rc = resource::alloc_handle(
        task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h
    );
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(obj);
        return syscall::EMFILE;
    }

    apply_cloexec(task, h, creation_flags);
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

    resource::set_status_flags(obj_a, static_cast<uint32_t>(creation_flags));
    resource::set_status_flags(obj_b, static_cast<uint32_t>(creation_flags));

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

    apply_cloexec(task, h0, creation_flags);
    apply_cloexec(task, h1, creation_flags);

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

    bool nonblock = (resource::get_status_flags(obj) & fs::O_NONBLOCK) != 0;
    int32_t result = sockops->connect(obj, kaddr, klen, nonblock);
    resource::resource_release(obj);
    return (result == resource::OK) ? 0 : syscall::error_map::map_socket_op_error(result);
}

DEFINE_SYSCALL3(accept, fd, addr, addrlen) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* listen_obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &listen_obj);
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

    bool nonblock = (resource::get_status_flags(listen_obj) & fs::O_NONBLOCK) != 0;

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

// Resolves a descriptor to a socket that can send (RIGHT_WRITE) or receive (RIGHT_READ)
__PRIVILEGED_CODE static int64_t lookup_socket(sched::task* task, uint64_t fd, uint32_t rights,
                                               socket_ref* out) {
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd),
        rights, &out->obj
    );
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    out->ops = resource::socket_ops_of(out->obj);
    bool sending = rights == resource::RIGHT_WRITE;
    if (!out->ops || (sending ? !out->ops->sendto : !out->ops->recvfrom)) {
        resource::resource_release(out->obj);
        return syscall::EOPNOTSUPP;
    }

    out->status_flags = resource::get_status_flags(out->obj);
    return 0;
}

__PRIVILEGED_CODE static int64_t copy_iovecs(uint64_t user_iov, uint64_t iovcnt,
                                             syscall::iovec** out_iovs, size_t* out_total) {
    if (iovcnt == 0) {
        return syscall::EINVAL;
    }

    if (iovcnt > syscall::MAX_IOVCNT) {
        return syscall::EMSGSIZE;
    }

    size_t bytes = static_cast<size_t>(iovcnt) * sizeof(syscall::iovec);
    auto* iovs = static_cast<syscall::iovec*>(heap::kzalloc(bytes));
    if (!iovs) {
        return syscall::ENOMEM;
    }

    if (mm::uaccess::copy_from_user(iovs, reinterpret_cast<const void*>(user_iov), bytes) != mm::uaccess::OK) {
        heap::kfree(iovs);
        return syscall::EFAULT;
    }

    size_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (__builtin_add_overflow(total, iovs[i].len, &total)) {
            heap::kfree(iovs);
            return syscall::EINVAL;
        }
    }

    *out_iovs = iovs;
    *out_total = total;
    return 0;
}

__PRIVILEGED_CODE static int64_t gather_from_user(const syscall::iovec* iovs, uint64_t iovcnt, uint8_t* data) {
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (iovs[i].len == 0) {
            continue;
        }

        if (mm::uaccess::copy_from_user(data, reinterpret_cast<const void*>(iovs[i].base), iovs[i].len) != mm::uaccess::OK) {
            return syscall::EFAULT;
        }

        data += iovs[i].len;
    }

    return 0;
}

__PRIVILEGED_CODE static int64_t scatter_to_user(const syscall::iovec* iovs, uint64_t iovcnt,
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

// A nonblocking descriptor never waits, whatever the call asked for
static uint32_t message_flags(const socket_ref& sock, uint64_t flags) {
    uint32_t msg_flags = static_cast<uint32_t>(flags);
    if (sock.status_flags & fs::O_NONBLOCK) {
        msg_flags |= net::inet::MSG_DONTWAIT;
    }

    return msg_flags;
}

__PRIVILEGED_CODE static int64_t copy_destination(uint64_t dest_addr, uint64_t addrlen, uint8_t* kaddr,
                                                  size_t* addr_len) {
    *addr_len = 0;
    if (dest_addr == 0 || addrlen == 0) {
        return 0;
    }

    if (addrlen > SENDTO_MAX_ADDR) {
        return syscall::EINVAL;
    }

    *addr_len = static_cast<size_t>(addrlen);
    if (mm::uaccess::copy_from_user(kaddr, reinterpret_cast<const void*>(dest_addr), *addr_len) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return 0;
}

// Sends kernel-resident data on the socket, to the named destination when there is one
__PRIVILEGED_CODE static int64_t send_on_socket(const socket_ref& sock, const uint8_t* data, size_t len,
                                                uint32_t flags, uint64_t dest_addr, uint64_t addrlen) {
    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t addr_len = 0;
    int64_t rc = copy_destination(dest_addr, addrlen, kaddr, &addr_len);
    if (rc != 0) {
        return rc;
    }

    ssize_t result = sock.ops->sendto(sock.obj, data, len, flags, kaddr, addr_len);
    if (result < 0) {
        return syscall::error_map::map_socket_op_error(static_cast<int32_t>(result));
    }

    return result;
}

__PRIVILEGED_CODE static int64_t send_stream(const socket_ref& sock, const syscall::iovec* iovs, uint64_t iovcnt,
                                             uint32_t flags, uint64_t dest_addr, uint64_t addrlen) {
    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t addr_len = 0;
    int64_t err = copy_destination(dest_addr, addrlen, kaddr, &addr_len);
    if (err != 0) {
        return err;
    }

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(syscall::STREAM_CHUNK_SIZE));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    int64_t total = 0;
    bool done = false;
    for (uint64_t i = 0; i < iovcnt && !done; i++) {
        size_t remaining = iovs[i].len;
        const uint8_t* user_ptr = reinterpret_cast<const uint8_t*>(iovs[i].base);

        while (remaining > 0) {
            size_t chunk = remaining > syscall::STREAM_CHUNK_SIZE ? syscall::STREAM_CHUNK_SIZE : remaining;
            if (mm::uaccess::copy_from_user(kbuf, user_ptr, chunk) != mm::uaccess::OK) {
                err = syscall::EFAULT;
                done = true;
                break;
            }

            ssize_t n = sock.ops->sendto(sock.obj, kbuf, chunk, flags, kaddr, addr_len);
            if (n < 0) {
                err = syscall::error_map::map_socket_op_error(static_cast<int32_t>(n));
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
    return total > 0 ? total : err;
}

__PRIVILEGED_CODE static int64_t receive_on_socket(const socket_ref& sock, uint8_t* data, size_t len,
                                                   uint32_t flags, uint8_t* kaddr, size_t* kaddr_len) {
    ssize_t result = sock.ops->recvfrom(sock.obj, data, len, flags, kaddr, kaddr_len);
    if (result < 0) {
        return syscall::error_map::map_socket_op_error(static_cast<int32_t>(result));
    }

    return result;
}

// Once a round has delivered bytes, the stream is asked again only while it is
// readable with no error pending, so an error is left for the next call to report
__PRIVILEGED_CODE static bool stream_has_more(const socket_ref& sock, uint32_t flags) {
    if (!sock.obj->ops->poll) {
        return true;
    }

    uint32_t ready = sock.obj->ops->poll(sock.obj, nullptr);
    if (ready & sync::POLL_ERR) {
        return false;
    }

    return (ready & sync::POLL_IN) || (flags & net::inet::MSG_WAITALL);
}

// A discard stages nothing, the socket drops the bytes itself
__PRIVILEGED_CODE static int64_t discard_stream(const socket_ref& sock, const syscall::iovec* iovs, uint64_t iovcnt,
                                                uint32_t flags, uint8_t* kaddr, size_t* kaddr_len) {
    size_t count = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        count += iovs[i].len;
    }

    ssize_t n = sock.ops->recvfrom(sock.obj, nullptr, count, flags, kaddr, kaddr_len);
    return n < 0 ? syscall::error_map::map_socket_op_error(static_cast<int32_t>(n)) : n;
}

// Each round peeks, copies to the caller, then discards what was copied unless
// the caller itself peeked, so a fault leaves the bytes queued. A peek is one
// round; otherwise later rounds take only what is queued unless MSG_WAITALL.
__PRIVILEGED_CODE static int64_t receive_stream(const socket_ref& sock, const syscall::iovec* iovs, uint64_t iovcnt,
                                                uint32_t flags, uint8_t* kaddr, size_t* kaddr_len) {
    if (flags & net::inet::MSG_TRUNC) {
        return discard_stream(sock, iovs, iovcnt, flags, kaddr, kaddr_len);
    }

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(syscall::STREAM_CHUNK_SIZE));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    bool peek = (flags & net::inet::MSG_PEEK) != 0;
    bool whole = (flags & net::inet::MSG_WAITALL) != 0 && !peek;
    uint32_t round_flags = (flags & ~net::inet::MSG_WAITALL) | net::inet::MSG_PEEK;
    size_t addr_capacity = *kaddr_len;
    int64_t total = 0;
    int64_t err = 0;
    bool done = false;
    for (uint64_t i = 0; i < iovcnt && !done; i++) {
        size_t remaining = iovs[i].len;
        uint8_t* user_ptr = reinterpret_cast<uint8_t*>(iovs[i].base);

        while (remaining > 0) {
            if (total > 0 && !stream_has_more(sock, flags)) {
                done = true;
                break;
            }

            size_t chunk = remaining > syscall::STREAM_CHUNK_SIZE ? syscall::STREAM_CHUNK_SIZE : remaining;
            size_t addr_len = addr_capacity;
            ssize_t n = sock.ops->recvfrom(sock.obj, kbuf, chunk, round_flags, kaddr, &addr_len);
            if (n < 0) {
                if (total == 0) {
                    err = syscall::error_map::map_socket_op_error(static_cast<int32_t>(n));
                }

                done = true;
                break;
            }

            if (total == 0) {
                *kaddr_len = addr_len;
            }

            if (n == 0) {
                done = true;
                break;
            }

            if (mm::uaccess::copy_to_user(user_ptr, kbuf, static_cast<size_t>(n)) != mm::uaccess::OK) {
                if (total == 0) {
                    err = syscall::EFAULT;
                }

                done = true;
                break;
            }

            if (!peek) {
                (void)sock.ops->recvfrom(sock.obj, nullptr, static_cast<size_t>(n),
                                         net::inet::MSG_TRUNC | net::inet::MSG_DONTWAIT, nullptr, nullptr);
            }

            total += n;
            user_ptr += n;
            remaining -= static_cast<size_t>(n);
            if (peek) {
                done = true;
                break;
            }

            if (!whole) {
                round_flags |= net::inet::MSG_DONTWAIT;
                if (static_cast<size_t>(n) < chunk) {
                    done = true;
                    break;
                }
            }
        }
    }

    heap::kfree(kbuf);
    return total > 0 ? total : err;
}

__PRIVILEGED_CODE static int64_t receive_datagram(const socket_ref& sock, uint64_t buf, size_t len, uint32_t flags,
                                                  uint8_t* kaddr, size_t* kaddr_len) {
    size_t staged = len < SENDTO_MAX_BUF ? len : SENDTO_MAX_BUF;
    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(staged));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    int64_t result = receive_on_socket(sock, kbuf, staged, flags, kaddr, kaddr_len);
    if (result >= 0 &&
        mm::uaccess::copy_to_user(reinterpret_cast<void*>(buf), kbuf, static_cast<size_t>(result)) != mm::uaccess::OK) {
        result = syscall::EFAULT;
    }

    heap::kfree(kbuf);
    return result;
}

__PRIVILEGED_CODE static int64_t copy_source_address(uint64_t user_addr, uint32_t user_len,
                                                     const uint8_t* kaddr, size_t kaddr_len) {
    size_t copy_len = kaddr_len < user_len ? kaddr_len : user_len;
    if (copy_len > 0 &&
        mm::uaccess::copy_to_user(reinterpret_cast<void*>(user_addr), kaddr, copy_len) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return 0;
}

__PRIVILEGED_CODE static int64_t send_datagram(const socket_ref& sock, uint64_t buf, size_t len, uint32_t flags,
                                               uint64_t dest_addr, uint64_t addrlen) {
    if (len > SENDTO_MAX_BUF) {
        return syscall::EMSGSIZE;
    }

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(len));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    int64_t result = syscall::EFAULT;
    if (mm::uaccess::copy_from_user(kbuf, reinterpret_cast<const void*>(buf), len) == mm::uaccess::OK) {
        result = send_on_socket(sock, kbuf, len, flags, dest_addr, addrlen);
    }

    heap::kfree(kbuf);
    return result;
}

DEFINE_SYSCALL6(sendto, fd, buf, len, flags, dest_addr, addrlen) {
    if (buf == 0 || len == 0) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    socket_ref sock;
    int64_t result = lookup_socket(task, fd, resource::RIGHT_WRITE, &sock);
    if (result != 0) {
        return result;
    }

    if (sock.ops->stream) {
        syscall::iovec whole = {buf, len};
        result = send_stream(sock, &whole, 1, message_flags(sock, flags), dest_addr, addrlen);
    } else {
        result = send_datagram(sock, buf, static_cast<size_t>(len), message_flags(sock, flags), dest_addr, addrlen);
    }

    resource::resource_release(sock.obj);
    return result;
}

DEFINE_SYSCALL3(sendmsg, fd, msg, flags) {
    if (msg == 0) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    msghdr hdr;
    if (mm::uaccess::copy_from_user(&hdr, reinterpret_cast<const void*>(msg), sizeof(hdr)) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    if (hdr.controllen != 0) {
        return syscall::EOPNOTSUPP;
    }

    syscall::iovec* iovs = nullptr;
    size_t data_len = 0;
    int64_t result = copy_iovecs(hdr.iov, hdr.iovlen, &iovs, &data_len);
    if (result != 0) {
        return result;
    }

    if (data_len == 0) {
        heap::kfree(iovs);
        return syscall::EINVAL;
    }

    socket_ref sock;
    result = lookup_socket(task, fd, resource::RIGHT_WRITE, &sock);
    if (result != 0) {
        heap::kfree(iovs);
        return result;
    }

    if (sock.ops->stream) {
        result = send_stream(sock, iovs, hdr.iovlen, message_flags(sock, flags), hdr.name, hdr.namelen);
    } else if (data_len > SENDTO_MAX_BUF) {
        result = syscall::EMSGSIZE;
    } else if (uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(data_len))) {
        result = gather_from_user(iovs, hdr.iovlen, kbuf);
        if (result == 0) {
            result = send_on_socket(sock, kbuf, data_len, message_flags(sock, flags), hdr.name, hdr.namelen);
        }

        heap::kfree(kbuf);
    } else {
        result = syscall::ENOMEM;
    }

    resource::resource_release(sock.obj);
    heap::kfree(iovs);
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

    socket_ref sock;
    int64_t result = lookup_socket(task, fd, resource::RIGHT_READ, &sock);
    if (result != 0) {
        return result;
    }

    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t kaddr_len = sizeof(kaddr);
    if (sock.ops->stream) {
        syscall::iovec whole = {buf, len};
        result = receive_stream(sock, &whole, 1, message_flags(sock, flags), kaddr, &kaddr_len);
    } else {
        result = receive_datagram(sock, buf, static_cast<size_t>(len), message_flags(sock, flags), kaddr, &kaddr_len);
    }

    resource::resource_release(sock.obj);
    if (result < 0 || src_addr == 0 || addrlen == 0) {
        return result;
    }

    uint32_t user_addrlen = 0;
    if (mm::uaccess::copy_from_user(&user_addrlen, reinterpret_cast<const void*>(addrlen), sizeof(user_addrlen)) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    int64_t addr_rc = copy_source_address(src_addr, user_addrlen, kaddr, kaddr_len);
    if (addr_rc != 0) {
        return addr_rc;
    }

    uint32_t out_len = static_cast<uint32_t>(kaddr_len);
    if (mm::uaccess::copy_to_user(reinterpret_cast<void*>(addrlen), &out_len, sizeof(out_len)) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return result;
}

DEFINE_SYSCALL3(recvmsg, fd, msg, flags) {
    if (msg == 0) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    msghdr hdr;
    if (mm::uaccess::copy_from_user(&hdr, reinterpret_cast<const void*>(msg), sizeof(hdr)) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    syscall::iovec* iovs = nullptr;
    size_t data_len = 0;
    int64_t result = copy_iovecs(hdr.iov, hdr.iovlen, &iovs, &data_len);
    if (result != 0) {
        return result;
    }

    if (data_len == 0) {
        heap::kfree(iovs);
        return syscall::EINVAL;
    }

    socket_ref sock;
    result = lookup_socket(task, fd, resource::RIGHT_READ, &sock);
    if (result != 0) {
        heap::kfree(iovs);
        return result;
    }

    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t kaddr_len = sizeof(kaddr);
    size_t staged = data_len < SENDTO_MAX_BUF ? data_len : SENDTO_MAX_BUF;
    if (sock.ops->stream) {
        result = receive_stream(sock, iovs, hdr.iovlen, message_flags(sock, flags), kaddr, &kaddr_len);
    } else if (uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(staged))) {
        result = receive_on_socket(sock, kbuf, staged, message_flags(sock, flags), kaddr, &kaddr_len);
        if (result >= 0) {
            int64_t scatter_rc = scatter_to_user(iovs, hdr.iovlen, kbuf, static_cast<size_t>(result));
            if (scatter_rc != 0) {
                result = scatter_rc;
            }
        }

        heap::kfree(kbuf);
    } else {
        result = syscall::ENOMEM;
    }

    resource::resource_release(sock.obj);
    heap::kfree(iovs);
    if (result < 0) {
        return result;
    }

    if (hdr.name != 0) {
        int64_t addr_rc = copy_source_address(hdr.name, hdr.namelen, kaddr, kaddr_len);
        if (addr_rc != 0) {
            return addr_rc;
        }
    }

    hdr.namelen = hdr.name != 0 ? static_cast<uint32_t>(kaddr_len) : 0;
    hdr.controllen = 0;
    hdr.flags = 0;
    if (mm::uaccess::copy_to_user(reinterpret_cast<void*>(msg), &hdr, sizeof(hdr)) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

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

    // SO_ERROR is the one option whose value is an errno, and the socket
    // reports it as a resource code that only this layer can translate
    if (level == net::inet::SOL_SOCKET && optname == net::inet::SO_ERROR && klen >= sizeof(int32_t)) {
        int32_t pending = 0;
        string::memcpy(&pending, kval, sizeof(pending));
        int32_t errnum = pending == resource::OK
            ? 0 : static_cast<int32_t>(-syscall::error_map::map_socket_op_error(pending));
        string::memcpy(kval, &errnum, sizeof(errnum));
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
