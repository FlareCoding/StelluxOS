#include "syscall/handlers/sys_eventfd.h"
#include "syscall/handlers/sys_error_map.h"
#include "eventfd/eventfd.h"
#include "resource/resource.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"

constexpr uint32_t EFD_SEMAPHORE = 0x1;
constexpr uint32_t EFD_NONBLOCK  = fs::O_NONBLOCK;
constexpr uint32_t EFD_CLOEXEC   = fs::O_CLOEXEC;
constexpr uint32_t EFD_FLAGS     = EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC;

DEFINE_SYSCALL2(eventfd2, initial_value, flags) {
    uint32_t efd_flags = static_cast<uint32_t>(flags);
    if (efd_flags & ~EFD_FLAGS) {
        return syscall::EINVAL;
    }

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* obj = nullptr;
    bool semaphore = (efd_flags & EFD_SEMAPHORE) != 0;
    if (eventfd::create(static_cast<uint32_t>(initial_value), semaphore, &obj) != resource::OK) {
        return syscall::ENOMEM;
    }

    resource::set_status_flags(obj, efd_flags & EFD_NONBLOCK);

    resource::handle_t handle = -1;
    int32_t rc = resource::alloc_task_handle(task, obj, resource::resource_type::EVENTFD,
                                             resource::RIGHT_READ | resource::RIGHT_WRITE, &handle);
    resource::resource_release(obj);

    if (rc != resource::HANDLE_OK) {
        return syscall::error_map::map_handle_alloc_error(rc);
    }

    if (efd_flags & EFD_CLOEXEC) {
        resource::set_handle_flags(task->handles, handle, resource::RESOURCE_HANDLE_CLOEXEC);
    }

    return handle;
}
