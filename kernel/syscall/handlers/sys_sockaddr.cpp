#include "syscall/handlers/sys_sockaddr.h"

#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"

// No socket family reports addresses yet, so both calls validate the
// descriptor and decline. A transport that carries addresses fills them in.
static int64_t reject_socket_address_query(uint64_t fd, uint64_t u_addr, uint64_t u_addrlen) {
    if (u_addr == 0 || u_addrlen == 0) {
        return syscall::EFAULT;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        task->handles, static_cast<resource::handle_t>(fd), 0, &obj);
    if (rc != resource::HANDLE_OK) {
        return syscall::EBADF;
    }

    bool is_socket = obj->type == resource::resource_type::SOCKET;
    resource::resource_release(obj);

    return is_socket ? syscall::EOPNOTSUPP : syscall::ENOTSOCK;
}

DEFINE_SYSCALL3(getsockname, fd, u_addr, u_addrlen) {
    return reject_socket_address_query(fd, u_addr, u_addrlen);
}

DEFINE_SYSCALL3(getpeername, fd, u_addr, u_addrlen) {
    return reject_socket_address_query(fd, u_addr, u_addrlen);
}
