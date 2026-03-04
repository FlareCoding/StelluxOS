#include "syscall/handlers/sys_pty.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "pty/pty.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"

DEFINE_SYSCALL1(pty_create, u_fds) {
    if (u_fds == 0) return syscall::EFAULT;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* master_obj = nullptr;
    resource::resource_object* slave_obj = nullptr;
    int32_t rc = pty::create_pair(&master_obj, &slave_obj);
    if (rc != resource::OK) {
        return syscall::ENOMEM;
    }

    resource::handle_t h0 = -1;
    rc = resource::alloc_handle(
        &task->handles, master_obj, resource::resource_type::PTY,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0);
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(master_obj);
        resource::resource_release(slave_obj);
        return syscall::EMFILE;
    }
    resource::resource_release(master_obj);

    resource::handle_t h1 = -1;
    rc = resource::alloc_handle(
        &task->handles, slave_obj, resource::resource_type::PTY,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1);
    if (rc != resource::HANDLE_OK) {
        resource::close(task, h0);
        resource::resource_release(slave_obj);
        return syscall::EMFILE;
    }
    resource::resource_release(slave_obj);

    int32_t kbuf[2] = {h0, h1};
    int32_t copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_fds), kbuf, sizeof(kbuf));
    if (copy_rc != mm::uaccess::OK) {
        resource::close(task, h1);
        resource::close(task, h0);
        return syscall::EFAULT;
    }

    return 0;
}
