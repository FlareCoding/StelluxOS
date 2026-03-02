#include "syscall/handlers/sys_socket.h"

#include "socket/unix_socket.h"
#include "socket/listener.h"
#include "fs/fs.h"
#include "fs/socket_node.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/heap.h"
#include "common/string.h"

namespace {

constexpr uint64_t AF_UNIX     = 1;
constexpr uint64_t SOCK_STREAM = 1;
constexpr size_t   SUN_PATH_OFFSET = 2;

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[socket::UNIX_PATH_MAX];
};

int64_t parse_sockaddr_un(uint64_t addr, uint64_t addrlen, char* kpath_out) {
    if (addrlen < sizeof(uint16_t)) {
        return syscall::EINVAL;
    }
    if (addr == 0) {
        return syscall::EFAULT;
    }

    sockaddr_un kaddr{};
    size_t copy_len = static_cast<size_t>(addrlen) < sizeof(sockaddr_un)
        ? static_cast<size_t>(addrlen) : sizeof(sockaddr_un);
    int32_t rc = mm::uaccess::copy_from_user(
        &kaddr, reinterpret_cast<const void*>(addr), copy_len);
    if (rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }
    if (kaddr.sun_family != AF_UNIX) {
        return syscall::EINVAL;
    }

    size_t path_max = copy_len - SUN_PATH_OFFSET;
    if (path_max == 0) {
        return syscall::EINVAL;
    }

    bool found_null = false;
    for (size_t i = 0; i < path_max; i++) {
        if (kaddr.sun_path[i] == '\0') {
            found_null = true;
            break;
        }
    }
    if (!found_null) {
        return syscall::EINVAL;
    }
    if (kaddr.sun_path[0] == '\0') {
        return syscall::EINVAL;
    }
    if (kaddr.sun_path[0] != '/') {
        return syscall::EINVAL;
    }

    string::memcpy(kpath_out, kaddr.sun_path, socket::UNIX_PATH_MAX);
    return 0;
}

} // anonymous namespace

DEFINE_SYSCALL3(socket, domain, type, protocol) {
    if (domain != AF_UNIX) {
        return syscall::EINVAL;
    }
    if (type != SOCK_STREAM) {
        return syscall::EINVAL;
    }
    if (protocol != 0) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = socket::create_unbound_socket(&obj);
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
    char kpath[socket::UNIX_PATH_MAX];
    int64_t parse_rc = parse_sockaddr_un(addr, addrlen, kpath);
    if (parse_rc != 0) {
        return parse_rc;
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
    if (obj->type != resource::resource_type::SOCKET || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }
    auto* sock = static_cast<socket::unix_socket*>(obj->impl);
    if (sock->state != socket::SOCK_STATE_UNBOUND) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    fs::node* parent = nullptr;
    const char* name = nullptr;
    size_t name_len = 0;
    rc = fs::resolve_parent_path(kpath, &parent, &name, &name_len);
    if (rc != fs::OK) {
        resource::resource_release(obj);
        if (rc == fs::ERR_NOENT) return syscall::ENOENT;
        if (rc == fs::ERR_NOTDIR) return syscall::ENOTDIR;
        return syscall::EINVAL;
    }

    fs::node* sock_node = nullptr;
    rc = parent->create_socket(name, name_len, nullptr, &sock_node);
    if (parent->release()) {
        fs::node::ref_destroy(parent);
    }
    if (rc != fs::OK) {
        resource::resource_release(obj);
        if (rc == fs::ERR_EXIST) return syscall::EADDRINUSE;
        if (rc == fs::ERR_NOMEM) return syscall::ENOMEM;
        return syscall::EIO;
    }

    string::memcpy(sock->bound_path, kpath, socket::UNIX_PATH_MAX);
    sock->bound_node = rc::strong_ref<fs::node>::adopt(sock_node);
    sock->state = socket::SOCK_STATE_BOUND;

    resource::resource_release(obj);
    return 0;
}

DEFINE_SYSCALL2(listen, fd, backlog) {
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
    if (obj->type != resource::resource_type::SOCKET || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }
    auto* sock = static_cast<socket::unix_socket*>(obj->impl);
    if (sock->state != socket::SOCK_STATE_BOUND) {
        resource::resource_release(obj);
        return syscall::EINVAL;
    }

    auto ls = rc::make_kref<socket::listener_state>();
    if (!ls) {
        resource::resource_release(obj);
        return syscall::ENOMEM;
    }
    ls->lock = sync::SPINLOCK_INIT;
    ls->closed = false;
    ls->accept_queue.init();
    ls->accept_wq.init();
    uint32_t bl = static_cast<uint32_t>(backlog);
    if (bl == 0) bl = socket::DEFAULT_BACKLOG;
    if (bl > socket::MAX_BACKLOG) bl = socket::MAX_BACKLOG;
    ls->backlog = bl;
    ls->pending_count = 0;

    sock->listener = ls;

    if (sock->bound_node) {
        auto* sn = static_cast<fs::socket_node*>(sock->bound_node.ptr());
        rc::strong_ref<socket::listener_state> ls_copy = ls;
        sn->set_listener(static_cast<rc::strong_ref<socket::listener_state>&&>(ls_copy));
    }

    sock->state = socket::SOCK_STATE_LISTENING;
    resource::resource_release(obj);
    return 0;
}

DEFINE_SYSCALL3(connect, fd, addr, addrlen) {
    char kpath[socket::UNIX_PATH_MAX];
    int64_t parse_rc = parse_sockaddr_un(addr, addrlen, kpath);
    if (parse_rc != 0) {
        return parse_rc;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* client_obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &client_obj
    );
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }
    if (client_obj->type != resource::resource_type::SOCKET || !client_obj->impl) {
        resource::resource_release(client_obj);
        return syscall::EINVAL;
    }
    auto* client_sock = static_cast<socket::unix_socket*>(client_obj->impl);
    if (client_sock->state != socket::SOCK_STATE_UNBOUND) {
        resource::resource_release(client_obj);
        return syscall::EISCONN;
    }

    fs::node* target_node = nullptr;
    rc = fs::lookup(kpath, &target_node);
    if (rc != fs::OK) {
        resource::resource_release(client_obj);
        if (rc == fs::ERR_NOENT) return syscall::ENOENT;
        return syscall::ECONNREFUSED;
    }
    if (target_node->type() != fs::node_type::socket) {
        if (target_node->release()) {
            fs::node::ref_destroy(target_node);
        }
        resource::resource_release(client_obj);
        return syscall::ECONNREFUSED;
    }

    auto* sn = static_cast<fs::socket_node*>(target_node);
    socket::listener_state* raw_ls = sn->get_listener();
    if (!raw_ls) {
        if (target_node->release()) {
            fs::node::ref_destroy(target_node);
        }
        resource::resource_release(client_obj);
        return syscall::ECONNREFUSED;
    }

    rc::strong_ref<socket::listener_state> ls_ref =
        rc::strong_ref<socket::listener_state>::try_from_raw(raw_ls);
    if (target_node->release()) {
        fs::node::ref_destroy(target_node);
    }
    if (!ls_ref) {
        resource::resource_release(client_obj);
        return syscall::ECONNREFUSED;
    }

    // Allocate everything BEFORE acquiring the listener lock
    auto chan = rc::make_kref<socket::unix_channel>();
    if (!chan) {
        resource::resource_release(client_obj);
        return syscall::ENOMEM;
    }
    chan->buf_a_to_b = nullptr;
    chan->buf_b_to_a = nullptr;

    chan->buf_a_to_b = socket::ring_buffer_create(socket::DEFAULT_CAPACITY);
    if (!chan->buf_a_to_b) {
        resource::resource_release(client_obj);
        return syscall::ENOMEM;
    }

    chan->buf_b_to_a = socket::ring_buffer_create(socket::DEFAULT_CAPACITY);
    if (!chan->buf_b_to_a) {
        resource::resource_release(client_obj);
        return syscall::ENOMEM;
    }

    auto* server_sock = heap::kalloc_new<socket::unix_socket>();
    if (!server_sock) {
        resource::resource_release(client_obj);
        return syscall::ENOMEM;
    }
    server_sock->state = socket::SOCK_STATE_CONNECTED;
    server_sock->lock = sync::SPINLOCK_INIT;
    server_sock->is_side_a = true;
    server_sock->channel = chan;

    auto* server_obj = heap::kalloc_new<resource::resource_object>();
    if (!server_obj) {
        heap::kfree_delete(server_sock);
        resource::resource_release(client_obj);
        return syscall::ENOMEM;
    }
    server_obj->type = resource::resource_type::SOCKET;
    server_obj->ops = socket::get_socket_ops();
    server_obj->impl = server_sock;

    auto* pc = static_cast<socket::pending_conn*>(
        heap::kzalloc(sizeof(socket::pending_conn)));
    if (!pc) {
        heap::kfree_delete(server_obj);
        heap::kfree_delete(server_sock);
        resource::resource_release(client_obj);
        return syscall::ENOMEM;
    }

    // Lock listener, re-check state, enqueue
    sync::irq_state irq = sync::spin_lock_irqsave(ls_ref->lock);
    if (ls_ref->closed || ls_ref->pending_count >= ls_ref->backlog) {
        sync::spin_unlock_irqrestore(ls_ref->lock, irq);
        heap::kfree(pc);
        heap::kfree_delete(server_obj);
        heap::kfree_delete(server_sock);
        resource::resource_release(client_obj);
        return syscall::ECONNREFUSED;
    }

    pc->server_obj = server_obj;
    ls_ref->accept_queue.push_back(pc);
    ls_ref->pending_count++;
    sync::spin_unlock_irqrestore(ls_ref->lock, irq);
    sync::wake_one(ls_ref->accept_wq);

    // Mutate client socket to CONNECTED (side B)
    client_sock->channel = static_cast<rc::strong_ref<socket::unix_channel>&&>(chan);
    client_sock->is_side_a = false;
    client_sock->state = socket::SOCK_STATE_CONNECTED;

    resource::resource_release(client_obj);
    return 0;
}

DEFINE_SYSCALL3(accept, fd, addr, addrlen) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* listen_obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd),
        resource::RIGHT_READ, &listen_obj
    );
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }
    if (listen_obj->type != resource::resource_type::SOCKET || !listen_obj->impl) {
        resource::resource_release(listen_obj);
        return syscall::EINVAL;
    }
    auto* sock = static_cast<socket::unix_socket*>(listen_obj->impl);
    if (sock->state != socket::SOCK_STATE_LISTENING || !sock->listener) {
        resource::resource_release(listen_obj);
        return syscall::EINVAL;
    }

    socket::listener_state* ls = sock->listener.ptr();

    sync::irq_state irq = sync::spin_lock_irqsave(ls->lock);
    while (ls->accept_queue.empty() && !ls->closed) {
        irq = sync::wait(ls->accept_wq, ls->lock, irq);
    }
    if (ls->accept_queue.empty()) {
        sync::spin_unlock_irqrestore(ls->lock, irq);
        resource::resource_release(listen_obj);
        return syscall::EINVAL;
    }

    socket::pending_conn* pc = ls->accept_queue.pop_front();
    ls->pending_count--;
    sync::spin_unlock_irqrestore(ls->lock, irq);

    resource::resource_object* server_obj = pc->server_obj;
    heap::kfree(pc);

    resource::handle_t new_handle = -1;
    rc = resource::alloc_handle(
        &task->handles, server_obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &new_handle
    );
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(server_obj);
        resource::resource_release(listen_obj);
        return syscall::EMFILE;
    }
    resource::resource_release(server_obj);
    resource::resource_release(listen_obj);

    if (addr != 0 && addrlen != 0) {
        uint16_t sun_family = static_cast<uint16_t>(AF_UNIX);
        mm::uaccess::copy_to_user(reinterpret_cast<void*>(addr), &sun_family, sizeof(sun_family));
        uint32_t out_len = sizeof(uint16_t);
        mm::uaccess::copy_to_user(reinterpret_cast<void*>(addrlen), &out_len, sizeof(out_len));
    }

    return new_handle;
}
