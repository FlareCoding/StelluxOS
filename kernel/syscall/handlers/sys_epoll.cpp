#include "syscall/handlers/sys_epoll.h"
#include "syscall/handlers/sys_error_map.h"
#include "syscall/handlers/sys_signal.h"
#include "syscall/epoll_event.h"
#include "epoll/epoll.h"
#include "resource/resource.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"

constexpr uint32_t EPOLL_CLOEXEC = fs::O_CLOEXEC;

constexpr int32_t EPOLL_CTL_ADD = 1;
constexpr int32_t EPOLL_CTL_DEL = 2;
constexpr int32_t EPOLL_CTL_MOD = 3;

constexpr int64_t NS_PER_MS = 1000000;

// The ABI bounds a wait's count so that its records fit in an int's worth of bytes
constexpr int64_t MAX_EVENT_BYTES = 0x7FFFFFFF;
constexpr int64_t MAX_EVENTS      = MAX_EVENT_BYTES / static_cast<int64_t>(sizeof(syscall::epoll_event));

static int64_t map_epoll_error(int32_t rc) {
    switch (rc) {
    case epoll::OK:        return 0;
    case epoll::ERR_INVAL: return syscall::EINVAL;
    case epoll::ERR_NOENT: return syscall::ENOENT;
    case epoll::ERR_EXIST: return syscall::EEXIST;
    case epoll::ERR_NOMEM: return syscall::ENOMEM;
    case epoll::ERR_PERM:  return syscall::EPERM;
    case epoll::ERR_NOSPC: return syscall::ENOSPC;
    case epoll::ERR_INTR:  return syscall::EINTR;
    default:               return syscall::EIO;
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static resource::resource_object* object_of(sched::task* task, uint64_t handle) {
    resource::resource_object* object = nullptr;
    int32_t rc = resource::get_handle_object(task->handles, static_cast<resource::handle_t>(handle), 0, &object);

    return rc == resource::HANDLE_OK ? object : nullptr;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int64_t create_epoll_handle(uint32_t flags) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    resource::resource_object* ep = nullptr;
    if (epoll::create(&ep) != epoll::OK) {
        return syscall::ENOMEM;
    }

    resource::handle_t handle = -1;
    int32_t rc = resource::alloc_task_handle(task, ep, resource::resource_type::EPOLL,
                                             resource::RIGHT_READ | resource::RIGHT_WRITE, &handle);
    resource::resource_release(ep);

    if (rc != resource::HANDLE_OK) {
        return syscall::error_map::map_handle_alloc_error(rc);
    }

    if (flags & EPOLL_CLOEXEC) {
        resource::set_handle_flags(task->handles, handle, resource::RESOURCE_HANDLE_CLOEXEC);
    }

    return handle;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int64_t apply_ctl(resource::resource_object* ep, int32_t op, resource::handle_t handle,
                                           resource::resource_object* target, const syscall::epoll_event& event) {
    // Regular files and directories are always ready, so the ABI refuses to watch them
    if (target->type == resource::resource_type::FILE) {
        return syscall::EPERM;
    }

    switch (op) {
    case EPOLL_CTL_ADD:
        return map_epoll_error(epoll::add_interest(ep, handle, target, event.events, event.data));
    case EPOLL_CTL_MOD:
        return map_epoll_error(epoll::modify_interest(ep, handle, target, event.events, event.data));
    case EPOLL_CTL_DEL:
        return map_epoll_error(epoll::remove_interest(ep, handle, target));
    default:
        return syscall::EINVAL;
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int64_t wait_and_report(sched::task* task, uint64_t epfd, uint64_t u_events,
                                                 int32_t max_events, int32_t timeout_ms) {
    if (max_events <= 0 || max_events > MAX_EVENTS) {
        return syscall::EINVAL;
    }

    uint32_t capacity = static_cast<uint32_t>(max_events);
    if (capacity > epoll::MAX_WAIT_EVENTS) {
        capacity = epoll::MAX_WAIT_EVENTS;
    }

    // Writing the buffer first refuses a bad one before the wait disarms a one-shot interest it reports
    syscall::epoll_event events[epoll::MAX_WAIT_EVENTS] = {};
    size_t capacity_bytes = capacity * sizeof(syscall::epoll_event);
    if (mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_events), events, capacity_bytes) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    resource::resource_object* ep = object_of(task, epfd);
    if (!ep) {
        return syscall::EBADF;
    }

    epoll::ready_event ready[epoll::MAX_WAIT_EVENTS];
    int64_t timeout_ns = static_cast<int64_t>(timeout_ms) * NS_PER_MS;
    int32_t reported = epoll::wait(ep, ready, capacity, timeout_ns);
    resource::resource_release(ep);

    if (reported <= 0) {
        return map_epoll_error(reported);
    }

    for (int32_t i = 0; i < reported; i++) {
        events[i].events = ready[i].events;
        events[i].data = ready[i].data;
    }

    size_t bytes = static_cast<size_t>(reported) * sizeof(syscall::epoll_event);
    if (mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_events), events, bytes) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return reported;
}

DEFINE_SYSCALL1(epoll_create, size) {
    if (static_cast<int32_t>(size) <= 0) {
        return syscall::EINVAL;
    }

    return create_epoll_handle(0);
}

DEFINE_SYSCALL1(epoll_create1, flags) {
    uint32_t create_flags = static_cast<uint32_t>(flags);
    if (create_flags & ~EPOLL_CLOEXEC) {
        return syscall::EINVAL;
    }

    return create_epoll_handle(create_flags);
}

DEFINE_SYSCALL4(epoll_ctl, epfd, op, fd, u_event) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    int32_t ctl_op = static_cast<int32_t>(op);
    syscall::epoll_event event = {};
    if (ctl_op != EPOLL_CTL_DEL) {
        int32_t rc = mm::uaccess::copy_from_user(&event, reinterpret_cast<const void*>(u_event), sizeof(event));
        if (rc != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    }

    resource::resource_object* ep = object_of(task, epfd);
    if (!ep) {
        return syscall::EBADF;
    }

    resource::resource_object* target = object_of(task, fd);
    if (!target) {
        resource::resource_release(ep);
        return syscall::EBADF;
    }

    int64_t result = apply_ctl(ep, ctl_op, static_cast<resource::handle_t>(fd), target, event);
    resource::resource_release(target);
    resource::resource_release(ep);

    return result;
}

DEFINE_SYSCALL4(epoll_wait, epfd, u_events, max_events, timeout_ms) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    return wait_and_report(task, epfd, u_events, static_cast<int32_t>(max_events), static_cast<int32_t>(timeout_ms));
}

DEFINE_SYSCALL6(epoll_pwait, epfd, u_events, max_events, timeout_ms, u_sigmask, sigsetsize) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    int64_t mask_result = syscall::set_temporary_sigmask(task, u_sigmask, sigsetsize);
    if (mask_result != 0) {
        return mask_result;
    }

    return wait_and_report(task, epfd, u_events, static_cast<int32_t>(max_events), static_cast<int32_t>(timeout_ms));
}
