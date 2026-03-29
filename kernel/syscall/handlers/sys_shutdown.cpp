#include "syscall/handlers/sys_shutdown.h"

#include "resource/resource.h"
#include "net/net.h"
#include "sched/sched.h"
#include "sched/task.h"

DEFINE_SYSCALL2(shutdown, fd, how) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    int32_t how_val = static_cast<int32_t>(how);
    if (how_val != net::SHUT_RD && how_val != net::SHUT_WR &&
        how_val != net::SHUT_RDWR) {
        return syscall::EINVAL;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(fd), 0, &obj);
    if (rc != resource::HANDLE_OK) return syscall::EBADF;

    if (obj->type != resource::resource_type::SOCKET) {
        resource::resource_release(obj);
        return syscall::ENOTSOCK;
    }

    if (!obj->ops || !obj->ops->shutdown) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
    }

    int32_t result = obj->ops->shutdown(obj, how_val);
    resource::resource_release(obj);

    if (result == resource::OK) return 0;
    if (result == resource::ERR_NOTCONN) return syscall::ENOTCONN;
    if (result == resource::ERR_INVAL) return syscall::EINVAL;
    return syscall::EIO;
}
