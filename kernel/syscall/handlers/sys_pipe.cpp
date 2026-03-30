#include "syscall/handlers/sys_pipe.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "pipe/pipe.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"

static int64_t do_pipe2(uint64_t u_fds, uint32_t flags) {
    if (u_fds == 0) return syscall::EFAULT;
    if (flags & ~static_cast<uint32_t>(fs::O_NONBLOCK)) return syscall::EINVAL;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    resource::resource_object* read_obj = nullptr;
    resource::resource_object* write_obj = nullptr;
    int32_t rc = pipe::create_pair(&read_obj, &write_obj);
    if (rc != resource::OK) {
        return syscall::ENOMEM;
    }

    resource::handle_t h_read = -1;
    rc = resource::alloc_handle(
        &task->handles, read_obj, resource::resource_type::PIPE,
        resource::RIGHT_READ, &h_read);
    if (rc != resource::HANDLE_OK) {
        resource::resource_release(read_obj);
        resource::resource_release(write_obj);
        return syscall::EMFILE;
    }
    resource::resource_release(read_obj);

    resource::handle_t h_write = -1;
    rc = resource::alloc_handle(
        &task->handles, write_obj, resource::resource_type::PIPE,
        resource::RIGHT_WRITE, &h_write);
    if (rc != resource::HANDLE_OK) {
        resource::close(task, h_read);
        resource::resource_release(write_obj);
        return syscall::EMFILE;
    }
    resource::resource_release(write_obj);

    if (flags & fs::O_NONBLOCK) {
        resource::set_handle_flags(&task->handles, h_read, fs::O_NONBLOCK);
        resource::set_handle_flags(&task->handles, h_write, fs::O_NONBLOCK);
    }

    int32_t kbuf[2] = {h_read, h_write};
    int32_t copy_rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_fds), kbuf, sizeof(kbuf));
    if (copy_rc != mm::uaccess::OK) {
        resource::close(task, h_write);
        resource::close(task, h_read);
        return syscall::EFAULT;
    }

    return 0;
}

DEFINE_SYSCALL1(pipe, u_fds) {
    return do_pipe2(u_fds, 0);
}

DEFINE_SYSCALL2(pipe2, u_fds, u_flags) {
    return do_pipe2(u_fds, static_cast<uint32_t>(u_flags));
}
