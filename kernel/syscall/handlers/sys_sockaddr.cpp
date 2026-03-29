#include "syscall/handlers/sys_sockaddr.h"

#include "resource/resource.h"
#include "net/tcp.h"
#include "net/inet_socket.h"
#include "net/byteorder.h"
#include "mm/uaccess.h"
#include "sched/sched.h"
#include "sched/task.h"

static void fill_inet_addr(
    net::kernel_sockaddr_in* out, uint32_t host_ip, uint16_t host_port
) {
    out->sin_family = net::AF_INET_VAL;
    out->sin_port = net::htons(host_port);
    out->sin_addr = net::htonl(host_ip);
}

DEFINE_SYSCALL3(getsockname, fd, u_addr, u_addrlen) {
    if (u_addr == 0 || u_addrlen == 0) return syscall::EFAULT;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd), 0, &obj);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;

    if (obj->type != resource::resource_type::SOCKET) {
        resource::resource_release(obj);
        return syscall::ENOTSOCK;
    }

    if (obj->ops != &net::g_tcp_ops || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
    }

    auto* sock = static_cast<net::tcp_socket*>(obj->impl);
    net::kernel_sockaddr_in kaddr = {};
    fill_inet_addr(&kaddr, sock->local_addr, sock->local_port);
    resource::resource_release(obj);

    uint32_t user_len = 0;
    int32_t copy_rc = mm::uaccess::copy_from_user(
        &user_len, reinterpret_cast<const void*>(u_addrlen), sizeof(user_len));
    if (copy_rc != mm::uaccess::OK) return syscall::EFAULT;

    uint32_t copy_len = user_len < sizeof(kaddr)
        ? user_len : static_cast<uint32_t>(sizeof(kaddr));
    if (copy_len > 0) {
        copy_rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(u_addr), &kaddr, copy_len);
        if (copy_rc != mm::uaccess::OK) return syscall::EFAULT;
    }

    uint32_t actual_len = static_cast<uint32_t>(sizeof(kaddr));
    copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_addrlen), &actual_len, sizeof(actual_len));
    if (copy_rc != mm::uaccess::OK) return syscall::EFAULT;

    return 0;
}

DEFINE_SYSCALL3(getpeername, fd, u_addr, u_addrlen) {
    if (u_addr == 0 || u_addrlen == 0) return syscall::EFAULT;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd), 0, &obj);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;

    if (obj->type != resource::resource_type::SOCKET) {
        resource::resource_release(obj);
        return syscall::ENOTSOCK;
    }

    if (obj->ops != &net::g_tcp_ops || !obj->impl) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
    }

    auto* sock = static_cast<net::tcp_socket*>(obj->impl);
    if (sock->remote_port == 0) {
        resource::resource_release(obj);
        return syscall::ENOTCONN;
    }

    net::kernel_sockaddr_in kaddr = {};
    fill_inet_addr(&kaddr, sock->remote_addr, sock->remote_port);
    resource::resource_release(obj);

    uint32_t user_len = 0;
    int32_t copy_rc = mm::uaccess::copy_from_user(
        &user_len, reinterpret_cast<const void*>(u_addrlen), sizeof(user_len));
    if (copy_rc != mm::uaccess::OK) return syscall::EFAULT;

    uint32_t copy_len = user_len < sizeof(kaddr)
        ? user_len : static_cast<uint32_t>(sizeof(kaddr));
    if (copy_len > 0) {
        copy_rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(u_addr), &kaddr, copy_len);
        if (copy_rc != mm::uaccess::OK) return syscall::EFAULT;
    }

    uint32_t actual_len = static_cast<uint32_t>(sizeof(kaddr));
    copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_addrlen), &actual_len, sizeof(actual_len));
    if (copy_rc != mm::uaccess::OK) return syscall::EFAULT;

    return 0;
}
