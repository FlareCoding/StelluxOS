#include "syscall/handlers/sys_socket.h"
#include "syscall/handlers/sys_error_map.h"
#include "syscall/handlers/sys_io.h"

#include "socket/unix_socket.h"
#include "net/inet.h"
#include "resource/socket_ops.h"
#include "resource/handle_batch.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/poll.h"
#include "sync/mutex.h"
#include "mm/uaccess.h"
#include "mm/heap.h"
#include "common/string.h"

constexpr uint64_t AF_UNIX     = 1;
constexpr uint64_t SOCK_STREAM = 1;
constexpr uint64_t SOCK_SEQPACKET = 5;
constexpr size_t   SENDTO_MAX_ADDR = 128;
constexpr uint64_t MAX_CONTROL_BYTES = 20480;
constexpr size_t   CMSG_ALIGNMENT    = 8;

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

// The header of one control message as userland lays it out, followed by its data
struct cmsghdr {
    uint64_t len;
    int32_t level;
    int32_t type;
};
static_assert(sizeof(cmsghdr) == 16);

// The SCM_RIGHTS message naming the handles one receive installed
struct rights_message {
    cmsghdr head;
    int32_t handles[resource::MAX_PASSED_HANDLES];
};

struct socket_ref {
    resource::resource_object* obj = nullptr;
    const resource::socket_ops* ops = nullptr;
    uint32_t status_flags = 0;
};

// Written over a caller's control buffer to fault it in before a receive takes anything
static const rights_message g_zeroed_rights = {};

__PRIVILEGED_CODE static void apply_cloexec(sched::task* task, resource::handle_t h, uint64_t creation_flags) {
    if (creation_flags & fs::O_CLOEXEC) {
        resource::set_handle_flags(task->handles, h, resource::RESOURCE_HANDLE_CLOEXEC);
    }
}

// The kernel's unix socket type for a type named in a system call, or false when unix sockets have none
static bool translate_unix_socket_type(uint64_t type, socket::unix_socket_type* out) {
    if (type == SOCK_STREAM) {
        *out = socket::unix_socket_type::stream;
        return true;
    }

    if (type == SOCK_SEQPACKET) {
        *out = socket::unix_socket_type::seqpacket;
        return true;
    }

    return false;
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
        socket::unix_socket_type unix_type = socket::unix_socket_type::stream;
        if (!translate_unix_socket_type(type, &unix_type) || protocol != 0) {
            return syscall::EINVAL;
        }

        rc = socket::create_unbound_socket(&obj, unix_type);
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
    rc = resource::alloc_task_handle(
        task, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h
    );
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(obj);
        return syscall::error_map::map_handle_alloc_error(rc);
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
    socket::unix_socket_type unix_type = socket::unix_socket_type::stream;
    if (!translate_unix_socket_type(type & ~SOCK_CREATION_FLAGS, &unix_type)) {
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
    int32_t rc = socket::create_socket_pair(&obj_a, &obj_b, unix_type);
    if (rc != resource::OK) {
        return syscall::ENOMEM;
    }

    resource::set_status_flags(obj_a, static_cast<uint32_t>(creation_flags));
    resource::set_status_flags(obj_b, static_cast<uint32_t>(creation_flags));

    resource::handle_t h0 = -1;
    rc = resource::alloc_task_handle(
        task, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0
    );
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(obj_a);
        resource::resource_release(obj_b);
        return syscall::error_map::map_handle_alloc_error(rc);
    }

    resource::resource_release(obj_a);

    resource::handle_t h1 = -1;
    rc = resource::alloc_task_handle(
        task, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1
    );
    if (rc != resource::HANDLE_OK) {
        resource::close(task, h0);
        resource::resource_release(obj_b);
        return syscall::error_map::map_handle_alloc_error(rc);
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
    rc = resource::alloc_task_handle(
        task, new_obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &new_handle);
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(new_obj);
        resource::resource_release(listen_obj);
        return syscall::error_map::map_handle_alloc_error(rc);
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

static size_t cmsg_align(size_t len) {
    return (len + CMSG_ALIGNMENT - 1) & ~(CMSG_ALIGNMENT - 1);
}

// The control message at `offset`, or nullptr once no whole header fits there
static const cmsghdr* control_message_at(const uint8_t* control, size_t size, size_t offset) {
    if (offset > size || size - offset < sizeof(cmsghdr)) {
        return nullptr;
    }

    return reinterpret_cast<const cmsghdr*>(control + offset);
}

// Whether a message may go out on the socket and how many handles it names. A socket that carries
// handles ignores other protocols' messages, and one that cannot ignores those only unix sockets act on.
static int64_t check_control_message(const cmsghdr* head, bool carries_handles, uint32_t* handles_named) {
    *handles_named = 0;
    bool socket_level = head->level == net::inet::SOL_SOCKET;

    if (!carries_handles) {
        bool unix_only = socket_level &&
                         (head->type == net::inet::SCM_RIGHTS || head->type == net::inet::SCM_CREDENTIALS);
        return unix_only ? 0 : syscall::EOPNOTSUPP;
    }

    if (!socket_level) {
        return 0;
    }

    if (head->type != net::inet::SCM_RIGHTS) {
        return syscall::EINVAL;
    }

    *handles_named = static_cast<uint32_t>((head->len - sizeof(cmsghdr)) / sizeof(int32_t));

    return 0;
}

// Checks every control message and counts the handles the SCM_RIGHTS messages name
static int64_t check_control_data(const uint8_t* control, size_t size, bool carries_handles, uint32_t* out_count) {
    uint32_t count = 0;
    size_t offset = 0;

    while (const cmsghdr* head = control_message_at(control, size, offset)) {
        if (head->len < sizeof(cmsghdr) || head->len > size - offset) {
            return syscall::EINVAL;
        }

        uint32_t named = 0;
        int64_t err = check_control_message(head, carries_handles, &named);
        if (err != 0) {
            return err;
        }

        if (named > resource::MAX_PASSED_HANDLES - count) {
            return syscall::EINVAL;
        }

        count += named;
        offset += cmsg_align(head->len);
    }

    *out_count = count;

    return 0;
}

// Looks up the `count` handles the SCM_RIGHTS messages name, filling a batch whose entries hold a reference
// to each object and the rights its handle grants
__PRIVILEGED_CODE static int64_t lookup_passed_handles(sched::task* task, const uint8_t* control, size_t size,
                                                       uint32_t count, resource::handle_batch** out_batch) {
    resource::handle_batch* batch = resource::create_handle_batch(count);
    if (!batch) {
        return syscall::ENOMEM;
    }

    uint32_t filled = 0;
    size_t offset = 0;

    while (const cmsghdr* head = control_message_at(control, size, offset)) {
        offset += cmsg_align(head->len);

        if (head->level != net::inet::SOL_SOCKET || head->type != net::inet::SCM_RIGHTS) {
            continue;
        }

        const auto* handles = reinterpret_cast<const int32_t*>(head + 1);
        size_t named = (head->len - sizeof(cmsghdr)) / sizeof(int32_t);

        for (size_t i = 0; i < named; i++) {
            resource::passed_handle& entry = batch->entries[filled];
            int32_t rc = resource::get_handle_object(task->handles, handles[i], 0, &entry.obj, nullptr, &entry.rights);
            if (rc != resource::HANDLE_OK) {
                resource::handle_batch_release(batch);
                return syscall::EBADF;
            }

            entry.type = entry.obj->type;
            filled++;
        }
    }

    *out_batch = batch;

    return 0;
}

// The handles the control data of a send passes, left null when it names none. Only a socket that
// carries handles collects them, and control data the socket cannot honor is refused.
__PRIVILEGED_CODE static int64_t collect_passed_handles(sched::task* task, const msghdr& hdr, bool carries_handles,
                                                        resource::handle_batch** out_batch) {
    *out_batch = nullptr;

    if (hdr.controllen == 0) {
        return 0;
    }

    if (hdr.controllen > MAX_CONTROL_BYTES) {
        return syscall::ENOBUFS;
    }

    size_t size = static_cast<size_t>(hdr.controllen);
    auto* control = static_cast<uint8_t*>(heap::uzalloc(size));
    if (!control) {
        return syscall::ENOMEM;
    }

    const void* user_control = reinterpret_cast<const void*>(hdr.control);
    bool copied = mm::uaccess::copy_from_user(control, user_control, size) == mm::uaccess::OK;

    uint32_t count = 0;
    int64_t err = copied ? check_control_data(control, size, carries_handles, &count) : syscall::EFAULT;

    if (err == 0 && count > 0) {
        err = lookup_passed_handles(task, control, size, count, out_batch);
    }

    heap::ufree(control);

    return err;
}

// How many handles an SCM_RIGHTS message fits in the caller's control buffer
static size_t handle_room(const msghdr& hdr) {
    return hdr.controllen > sizeof(cmsghdr) ? (hdr.controllen - sizeof(cmsghdr)) / sizeof(int32_t) : 0;
}

// Installs handles from a received batch, at most `room`, recording each one installed in `handles`.
// Installing stops at the first handle the table has no room for.
__PRIVILEGED_CODE static uint32_t install_passed_handles(sched::task* task, const resource::handle_batch* batch,
                                                         size_t room, bool cloexec, int32_t* handles) {
    uint32_t installed = 0;

    while (installed < batch->count && installed < room) {
        const resource::passed_handle& entry = batch->entries[installed];
        resource::handle_t handle = -1;
        if (resource::alloc_task_handle(task, entry.obj, entry.type, entry.rights, &handle) != resource::HANDLE_OK) {
            break;
        }

        if (cloexec) {
            resource::set_handle_flags(task->handles, handle, resource::RESOURCE_HANDLE_CLOEXEC);
        }

        handles[installed] = handle;
        installed++;
    }

    return installed;
}

__PRIVILEGED_CODE static void close_handles(sched::task* task, const int32_t* handles, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        (void)resource::close(task, handles[i]);
    }
}

// Installs the handles a receive took, as many as the caller's control buffer has room for, and stages the
// SCM_RIGHTS message naming them, describing it in the header with MSG_CTRUNC for any dropped
__PRIVILEGED_CODE static uint32_t deliver_handles(sched::task* task, resource::handle_batch* batch, bool cloexec,
                                                  rights_message* message, msghdr* hdr) {
    uint32_t installed = batch ? install_passed_handles(task, batch, handle_room(*hdr), cloexec, message->handles) : 0;

    message->head = {sizeof(cmsghdr) + installed * sizeof(int32_t), net::inet::SOL_SOCKET, net::inet::SCM_RIGHTS};
    uint64_t message_space = cmsg_align(message->head.len);

    hdr->controllen = installed == 0 ? 0 : (message_space < hdr->controllen ? message_space : hdr->controllen);
    hdr->flags = batch && installed < batch->count ? net::inet::MSG_CTRUNC : 0;

    resource::handle_batch_release(batch);

    return installed;
}

// Copies the staged SCM_RIGHTS message to the caller's control buffer when it names any handles
__PRIVILEGED_CODE static bool copy_rights_message(const msghdr& hdr, const rights_message& message,
                                                  uint32_t installed) {
    void* user_control = reinterpret_cast<void*>(hdr.control);
    return installed == 0 || mm::uaccess::copy_to_user(user_control, &message, message.head.len) == mm::uaccess::OK;
}

// Writes the header back unchanged and zeroes the control buffer's room for a rights message, so a receive
// into buffers it cannot write fails before it takes anything
__PRIVILEGED_CODE static bool fault_in_header_and_control(uint64_t msg, const msghdr& hdr) {
    if (mm::uaccess::copy_to_user(reinterpret_cast<void*>(msg), &hdr, sizeof(hdr)) != mm::uaccess::OK) {
        return false;
    }

    if (handle_room(hdr) == 0) {
        return true;
    }

    size_t reach = hdr.controllen < sizeof(rights_message) ? hdr.controllen : sizeof(rights_message);
    void* user_control = reinterpret_cast<void*>(hdr.control);

    return mm::uaccess::copy_to_user(user_control, &g_zeroed_rights, reach) == mm::uaccess::OK;
}

// Gives the batch a receive took to a caller that asked for it, and drops it otherwise
__PRIVILEGED_CODE static void hand_over_batch(resource::handle_batch* batch, resource::handle_batch** out_batch) {
    if (out_batch) {
        *out_batch = batch;
    } else {
        resource::handle_batch_release(batch);
    }
}

// Sends kernel-resident data on the socket with `batch`, to the named destination when there is one
__PRIVILEGED_CODE static int64_t send_on_socket(const socket_ref& sock, const uint8_t* data, size_t len,
                                                uint32_t flags, uint64_t dest_addr, uint64_t addrlen,
                                                resource::handle_batch* batch) {
    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t addr_len = 0;
    int64_t rc = copy_destination(dest_addr, addrlen, kaddr, &addr_len);
    if (rc != 0) {
        return rc;
    }

    ssize_t result = batch ? sock.ops->sendmsg(sock.obj, data, len, flags, kaddr, addr_len, batch)
                           : sock.ops->sendto(sock.obj, data, len, flags, kaddr, addr_len);
    if (result < 0) {
        return syscall::error_map::map_socket_op_error(static_cast<int32_t>(result));
    }

    return result;
}

// A batch rides on the first chunk and belongs to the socket once any of its bytes went out, while
// a batch no byte went out with is dropped here
__PRIVILEGED_CODE static int64_t send_stream(const socket_ref& sock, const syscall::iovec* iovs, uint64_t iovcnt,
                                             uint32_t flags, uint64_t dest_addr, uint64_t addrlen,
                                             resource::handle_batch* batch) {
    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t addr_len = 0;
    int64_t err = copy_destination(dest_addr, addrlen, kaddr, &addr_len);
    if (err != 0) {
        resource::handle_batch_release(batch);
        return err;
    }

    uint8_t* kbuf = static_cast<uint8_t*>(heap::kzalloc(syscall::STREAM_CHUNK_SIZE));
    if (!kbuf) {
        resource::handle_batch_release(batch);
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

            ssize_t n = batch ? sock.ops->sendmsg(sock.obj, kbuf, chunk, flags, kaddr, addr_len, batch)
                              : sock.ops->sendto(sock.obj, kbuf, chunk, flags, kaddr, addr_len);
            if (n < 0) {
                err = syscall::error_map::map_socket_op_error(static_cast<int32_t>(n));
                done = true;
                break;
            }

            batch = nullptr;

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
    resource::handle_batch_release(batch);

    return total > 0 ? total : err;
}

__PRIVILEGED_CODE static int64_t receive_on_socket(const socket_ref& sock, uint8_t* data, size_t len,
                                                   uint32_t flags, uint8_t* kaddr, size_t* kaddr_len,
                                                   resource::handle_batch** out_batch) {
    ssize_t result = out_batch ? sock.ops->recvmsg(sock.obj, data, len, flags, kaddr, kaddr_len, out_batch)
                               : sock.ops->recvfrom(sock.obj, data, len, flags, kaddr, kaddr_len);
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

// The lock a stream's receives hold across all their rounds, or nullptr when it has none
__PRIVILEGED_CODE static sync::mutex* receive_lock_of(const socket_ref& sock) {
    return sock.ops->receive_lock ? sock.ops->receive_lock(sock.obj) : nullptr;
}

__PRIVILEGED_CODE static void lock_receives(sync::mutex* lock) {
    if (lock) {
        sync::mutex_lock(*lock);
    }
}

__PRIVILEGED_CODE static void unlock_receives(sync::mutex* lock) {
    if (lock) {
        sync::mutex_unlock(*lock);
    }
}

// Receives through the handle-carrying operation when the caller wants the batch it may pass
__PRIVILEGED_CODE static ssize_t receive_attempt(const socket_ref& sock, void* kbuf, size_t len, uint32_t flags,
                                                 uint8_t* kaddr, size_t* addr_len, resource::handle_batch** batch) {
    if (batch && sock.ops->recvmsg) {
        return sock.ops->recvmsg(sock.obj, kbuf, len, flags, kaddr, addr_len, batch);
    }

    return sock.ops->recvfrom(sock.obj, kbuf, len, flags, kaddr, addr_len);
}

// A round never waits under the receive lock. When nothing is queued and the caller may
// wait, it lets go of the lock while waiting, so a sleeping receive never holds up the others.
__PRIVILEGED_CODE static ssize_t take_round(const socket_ref& sock, sync::mutex* lock, void* kbuf, size_t len,
                                            uint32_t round_flags, uint8_t* kaddr, size_t* addr_len,
                                            resource::handle_batch** batch) {
    while (true) {
        ssize_t n = receive_attempt(sock, kbuf, len, round_flags | net::inet::MSG_DONTWAIT, kaddr, addr_len, batch);
        if (n != resource::ERR_AGAIN || (round_flags & net::inet::MSG_DONTWAIT)) {
            return n;
        }

        // A blocking one-byte peek is how any stream waits for bytes without taking them
        uint8_t probe = 0;
        uint32_t wait_flags = (round_flags & ~net::inet::MSG_TRUNC) | net::inet::MSG_PEEK;

        unlock_receives(lock);
        ssize_t ready = sock.ops->recvfrom(sock.obj, &probe, 1, wait_flags, nullptr, nullptr);
        lock_receives(lock);

        if (ready <= 0) {
            return ready;
        }
    }
}

// A discard stages nothing, the socket drops the bytes itself. Under MSG_WAITALL it
// keeps dropping until the whole length is gone or the stream ends.
__PRIVILEGED_CODE static int64_t discard_stream(const socket_ref& sock, const syscall::iovec* iovs, uint64_t iovcnt,
                                                uint32_t flags, uint8_t* kaddr, size_t* kaddr_len,
                                                resource::handle_batch** out_batch) {
    size_t count = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        count += iovs[i].len;
    }

    // A stream asked for no bytes may answer as if nothing were queued, so a receive with no room returns here
    if (count == 0) {
        return 0;
    }

    bool peek = (flags & net::inet::MSG_PEEK) != 0;
    bool whole = (flags & net::inet::MSG_WAITALL) != 0 && !peek;
    uint32_t round_flags = flags & ~net::inet::MSG_WAITALL;

    sync::mutex* lock = receive_lock_of(sock);
    lock_receives(lock);

    // A peek shares a batch only with a caller that wants it, but a discard must see any batch it consumes
    resource::handle_batch* batch = nullptr;
    resource::handle_batch** round_batch = peek && !out_batch ? nullptr : &batch;

    ssize_t n = take_round(sock, lock, nullptr, count, round_flags, kaddr, kaddr_len, round_batch);
    size_t dropped = n > 0 ? static_cast<size_t>(n) : 0;

    // The bytes a batch came with end the receive, even under MSG_WAITALL
    while (whole && n > 0 && !batch && dropped < count) {
        n = take_round(sock, lock, nullptr, count - dropped, round_flags, nullptr, nullptr, &batch);
        dropped += n > 0 ? static_cast<size_t>(n) : 0;
    }

    unlock_receives(lock);

    hand_over_batch(batch, out_batch);

    if (dropped > 0) {
        return static_cast<int64_t>(dropped);
    }

    return n < 0 ? syscall::error_map::map_socket_op_error(static_cast<int32_t>(n)) : 0;
}

// Consumes what a round copied out, returning the batch those bytes came with, if any
[[nodiscard]] __PRIVILEGED_CODE static resource::handle_batch* consume_round(const socket_ref& sock, size_t len) {
    resource::handle_batch* batch = nullptr;
    (void)receive_attempt(sock, nullptr, len, net::inet::MSG_TRUNC | net::inet::MSG_DONTWAIT, nullptr, nullptr,
                          &batch);
    return batch;
}

// Each round peeks, copies to the caller, then discards what was copied unless the caller
// peeked, so a fault leaves the bytes queued, all under the stream's receive lock. A peek is
// one round, and otherwise later rounds take only what is queued unless MSG_WAITALL.
__PRIVILEGED_CODE static int64_t receive_stream(const socket_ref& sock, const syscall::iovec* iovs, uint64_t iovcnt,
                                                uint32_t flags, uint8_t* kaddr, size_t* kaddr_len,
                                                resource::handle_batch** out_batch) {
    if (flags & net::inet::MSG_TRUNC) {
        return discard_stream(sock, iovs, iovcnt, flags, kaddr, kaddr_len, out_batch);
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

    // Only the caller's own peek shares a batch, never the peek that stages a consuming round
    resource::handle_batch* batch = nullptr;
    resource::handle_batch** round_batch = peek && out_batch ? &batch : nullptr;

    sync::mutex* lock = receive_lock_of(sock);
    lock_receives(lock);

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
            ssize_t n = take_round(sock, lock, kbuf, chunk, round_flags, kaddr, &addr_len, round_batch);
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
                batch = consume_round(sock, static_cast<size_t>(n));
            }

            total += n;
            user_ptr += n;
            remaining -= static_cast<size_t>(n);

            // The bytes a batch came with end the receive
            if (peek || batch) {
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

    unlock_receives(lock);

    hand_over_batch(batch, out_batch);
    heap::kfree(kbuf);

    return total > 0 ? total : err;
}

// Receives one message and, given `out_batch`, its handles. Returns the bytes copied into `iovs`, or the
// full length when MSG_TRUNC asks for it, and reports through `truncated` a message that lost its tail.
__PRIVILEGED_CODE static int64_t receive_whole_message(const socket_ref& sock, const syscall::iovec* iovs,
                                                       uint64_t iovcnt, size_t len, uint32_t flags,
                                                       uint8_t* kaddr, size_t* kaddr_len, bool* truncated,
                                                       resource::handle_batch** out_batch) {
    size_t staged = len < sock.ops->max_message ? len : sock.ops->max_message;

    // Never empty, since the heap refuses empty requests and a receive with no room must still take its message
    auto* kbuf = static_cast<uint8_t*>(heap::uzalloc(staged > 0 ? staged : 1));
    if (!kbuf) {
        return syscall::ENOMEM;
    }

    int64_t result = receive_on_socket(sock, kbuf, staged, flags, kaddr, kaddr_len, out_batch);
    if (result >= 0) {
        size_t copied = static_cast<size_t>(result) < staged ? static_cast<size_t>(result) : staged;
        *truncated = static_cast<size_t>(result) > staged;

        if (syscall::scatter_to_user(iovs, iovcnt, kbuf, copied) != 0) {
            result = syscall::EFAULT;
        } else if (!(flags & net::inet::MSG_TRUNC)) {
            result = static_cast<int64_t>(copied);
        }
    }

    heap::ufree(kbuf);

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

// Sends the user buffers `iovs` describe as one message carrying `batch`, to the named destination when there
// is one. The socket owns the batch once the message is sent, and it is dropped otherwise.
__PRIVILEGED_CODE static int64_t send_whole_message(const socket_ref& sock, const syscall::iovec* iovs,
                                                    uint64_t iovcnt, size_t len, uint32_t flags,
                                                    uint64_t dest_addr, uint64_t addrlen,
                                                    resource::handle_batch* batch) {
    if (len > sock.ops->max_message) {
        resource::handle_batch_release(batch);
        return syscall::EMSGSIZE;
    }

    auto* kbuf = static_cast<uint8_t*>(heap::uzalloc(len));
    if (!kbuf) {
        resource::handle_batch_release(batch);
        return syscall::ENOMEM;
    }

    int64_t result = syscall::gather_from_user(iovs, iovcnt, kbuf);
    if (result == 0) {
        result = send_on_socket(sock, kbuf, len, flags, dest_addr, addrlen, batch);
    }

    heap::ufree(kbuf);

    if (result <= 0) {
        resource::handle_batch_release(batch);
    }

    return result;
}

// A send of no bytes lets the socket decide what it means, reporting a broken connection or refusing
// an empty message, and the handles it names are dropped, since no byte carries them
__PRIVILEGED_CODE static int64_t send_zero_bytes(const socket_ref& sock, uint32_t flags, uint64_t dest_addr,
                                                 uint64_t addrlen, resource::handle_batch* batch) {
    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t addr_len = 0;
    int64_t err = copy_destination(dest_addr, addrlen, kaddr, &addr_len);

    if (err == 0) {
        uint8_t unused_byte = 0;
        ssize_t n = batch ? sock.ops->sendmsg(sock.obj, &unused_byte, 0, flags, kaddr, addr_len, batch)
                          : sock.ops->sendto(sock.obj, &unused_byte, 0, flags, kaddr, addr_len);
        err = n < 0 ? syscall::error_map::map_socket_op_error(static_cast<int32_t>(n)) : 0;
    }

    resource::handle_batch_release(batch);

    return err;
}

DEFINE_SYSCALL6(sendto, fd, buf, len, flags, dest_addr, addrlen) {
    if (buf == 0 && len != 0) {
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

    uint32_t send_flags = message_flags(sock, flags);
    syscall::iovec whole = {buf, len};
    if (len == 0) {
        result = send_zero_bytes(sock, send_flags, dest_addr, addrlen, nullptr);
    } else if (!sock.ops->stream) {
        result = send_whole_message(sock, &whole, 1, static_cast<size_t>(len), send_flags, dest_addr,
                                    addrlen, nullptr);
    } else {
        result = send_stream(sock, &whole, 1, send_flags, dest_addr, addrlen, nullptr);
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

    syscall::iovec* iovs = nullptr;
    size_t data_len = 0;
    int64_t result = copy_iovecs(hdr.iov, hdr.iovlen, &iovs, &data_len);
    if (result != 0) {
        return result;
    }

    socket_ref sock;
    result = lookup_socket(task, fd, resource::RIGHT_WRITE, &sock);
    if (result != 0) {
        heap::kfree(iovs);
        return result;
    }

    resource::handle_batch* batch = nullptr;
    result = collect_passed_handles(task, hdr, sock.ops->sendmsg != nullptr, &batch);

    if (result == 0) {
        uint32_t send_flags = message_flags(sock, flags);

        if (data_len == 0) {
            result = send_zero_bytes(sock, send_flags, hdr.name, hdr.namelen, batch);
        } else if (!sock.ops->stream) {
            result = send_whole_message(sock, iovs, hdr.iovlen, data_len, send_flags, hdr.name,
                                        hdr.namelen, batch);
        } else {
            result = send_stream(sock, iovs, hdr.iovlen, send_flags, hdr.name, hdr.namelen, batch);
        }
    }

    resource::resource_release(sock.obj);
    heap::kfree(iovs);

    return result;
}

DEFINE_SYSCALL6(recvfrom, fd, buf, len, flags, src_addr, addrlen) {
    if (buf == 0 && len != 0) {
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
    syscall::iovec whole = {buf, len};
    if (sock.ops->stream) {
        result = receive_stream(sock, &whole, 1, message_flags(sock, flags), kaddr, &kaddr_len, nullptr);
    } else {
        bool truncated = false;
        result = receive_whole_message(sock, &whole, 1, static_cast<size_t>(len), message_flags(sock, flags),
                                       kaddr, &kaddr_len, &truncated, nullptr);
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

    socket_ref sock;
    result = lookup_socket(task, fd, resource::RIGHT_READ, &sock);
    if (result != 0) {
        heap::kfree(iovs);
        return result;
    }

    // Faulted in before anything is taken, so an unwritable buffer leaves the message and its handles queued
    if (sock.ops->recvmsg && !fault_in_header_and_control(msg, hdr)) {
        resource::resource_release(sock.obj);
        heap::kfree(iovs);
        return syscall::EFAULT;
    }

    bool cloexec = (flags & net::inet::MSG_CMSG_CLOEXEC) != 0;
    uint32_t receive_flags = message_flags(sock, flags & ~static_cast<uint64_t>(net::inet::MSG_CMSG_CLOEXEC));

    uint8_t kaddr[SENDTO_MAX_ADDR] = {};
    size_t kaddr_len = sizeof(kaddr);
    resource::handle_batch* batch = nullptr;
    resource::handle_batch** out_batch = sock.ops->recvmsg ? &batch : nullptr;
    bool truncated = false;

    if (sock.ops->stream) {
        result = receive_stream(sock, iovs, hdr.iovlen, receive_flags, kaddr, &kaddr_len, out_batch);
    } else {
        result = receive_whole_message(sock, iovs, hdr.iovlen, data_len, receive_flags, kaddr, &kaddr_len,
                                       &truncated, out_batch);
    }

    resource::resource_release(sock.obj);
    heap::kfree(iovs);

    if (result < 0) {
        resource::handle_batch_release(batch);
        return result;
    }

    if (hdr.name != 0) {
        int64_t addr_rc = copy_source_address(hdr.name, hdr.namelen, kaddr, kaddr_len);
        if (addr_rc != 0) {
            resource::handle_batch_release(batch);
            return addr_rc;
        }
    }

    // Staged on the stack, since no memory may be claimed once the bytes carrying the handles are taken
    rights_message message;
    uint32_t installed = deliver_handles(task, batch, cloexec, &message, &hdr);
    hdr.namelen = hdr.name != 0 ? static_cast<uint32_t>(kaddr_len) : 0;

    if (truncated) {
        hdr.flags |= net::inet::MSG_TRUNC;
    }

    bool reported = copy_rights_message(hdr, message, installed) &&
                    mm::uaccess::copy_to_user(reinterpret_cast<void*>(msg), &hdr, sizeof(hdr)) == mm::uaccess::OK;

    // A receive that cannot report what it took fails, keeping none of the handles it installed
    if (!reported) {
        close_handles(task, message.handles, installed);
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
