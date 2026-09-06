#include "syscall/handlers/sys_sockaddr.h"
#include "syscall/handlers/sys_error_map.h"

#include "resource/socket_ops.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"

constexpr size_t SOCKADDR_STORAGE_LEN = 128; // Largest address any family reports

static int64_t query_socket_address(uint64_t fd, uint64_t u_addr, uint64_t u_addrlen, bool peer) {
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

    const resource::socket_ops* sockops = resource::socket_ops_of(obj);
    if (!sockops) {
        resource::resource_release(obj);
        return syscall::ENOTSOCK;
    }

    if (!sockops->getname) {
        resource::resource_release(obj);
        return syscall::EOPNOTSUPP;
    }

    uint8_t kaddr[SOCKADDR_STORAGE_LEN] = {};
    size_t kaddr_len = sizeof(kaddr);
    
    int32_t result = sockops->getname(obj, kaddr, &kaddr_len, peer);
    resource::resource_release(obj);
    
    if (result != resource::OK) {
        return syscall::error_map::map_socket_op_error(result);
    }

    // The caller gets as much of the address as fits,
    // and the full length so a short buffer is detectable.
    uint32_t user_addrlen = 0;
    if (mm::uaccess::copy_from_user(&user_addrlen, reinterpret_cast<const void*>(u_addrlen),
                                    sizeof(user_addrlen)) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    size_t copy_len = kaddr_len < user_addrlen ? kaddr_len : user_addrlen;
    if (copy_len > 0 &&
        mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_addr), kaddr, copy_len) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    uint32_t out_len = static_cast<uint32_t>(kaddr_len);
    if (mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_addrlen), &out_len,
                                  sizeof(out_len)) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return 0;
}

DEFINE_SYSCALL3(getsockname, fd, u_addr, u_addrlen) {
    return query_socket_address(fd, u_addr, u_addrlen, false);
}

DEFINE_SYSCALL3(getpeername, fd, u_addr, u_addrlen) {
    return query_socket_address(fd, u_addr, u_addrlen, true);
}
