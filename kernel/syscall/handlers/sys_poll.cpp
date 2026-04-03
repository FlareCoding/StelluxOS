#include "syscall/handlers/sys_poll.h"

#include "sync/poll.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/heap.h"

struct kernel_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct kernel_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

constexpr size_t MAX_POLL_FDS = 256;
constexpr int64_t NS_PER_MS  = 1000000LL;
constexpr int64_t NS_PER_SEC = 1000000000LL;

// Unconditional output events reported regardless of what the user requested.
constexpr int16_t ALWAYS_EVENTS =
    static_cast<int16_t>(sync::POLL_ERR | sync::POLL_HUP | sync::POLL_NVAL);

__PRIVILEGED_CODE static int64_t poll_one_fd(
    sched::task* task, kernel_pollfd& pfd, sync::poll_table* pt
) {
    pfd.revents = 0;

    if (pfd.fd < 0) {
        return 0;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = resource::get_handle_object(
        &task->handles, static_cast<resource::handle_t>(pfd.fd), 0, &obj);
    if (rc != resource::HANDLE_OK) {
        pfd.revents = static_cast<int16_t>(sync::POLL_NVAL);
        return 1;
    }

    if (!obj->ops || !obj->ops->poll) {
        pfd.revents = static_cast<int16_t>(sync::POLL_NVAL);
        resource::resource_release(obj);
        return 1;
    }

    uint32_t mask = obj->ops->poll(obj, pt);
    resource::resource_release(obj);

    pfd.revents = static_cast<int16_t>(mask) & (pfd.events | ALWAYS_EVENTS);
    return pfd.revents != 0 ? 1 : 0;
}

__PRIVILEGED_CODE static int64_t do_poll(
    sched::task* task,
    kernel_pollfd* kfds, uint32_t nfds,
    uint64_t timeout_ns, bool infinite, bool immediate
) {
    sync::poll_table pt;
    pt.init(task);

    int64_t ready = 0;

    // First pass: check readiness + subscribe (unless immediate/non-blocking)
    for (uint32_t i = 0; i < nfds; i++) {
        ready += poll_one_fd(task, kfds[i], immediate ? nullptr : &pt);
    }

    if (__atomic_load_n(&pt.error, __ATOMIC_ACQUIRE)) {
        sync::poll_cleanup(pt);
        return syscall::ENOMEM;
    }

    if (ready > 0 || immediate) {
        sync::poll_cleanup(pt);
        return ready;
    }

    // nfds can be 0 with a timeout (sleep idiom)
    if (!infinite || nfds > 0) {
        sync::poll_wait(pt, infinite ? 0 : timeout_ns);
    } else {
        // nfds == 0, infinite: block until kill
        sync::poll_wait(pt, 0);
    }

    // Re-check pass (probe only, no subscribe)
    ready = 0;
    for (uint32_t i = 0; i < nfds; i++) {
        ready += poll_one_fd(task, kfds[i], nullptr);
    }

    sync::poll_cleanup(pt);
    return ready;
}

DEFINE_SYSCALL5(ppoll, u_fds, nfds_val, u_timeout, u_sigmask, sigsetsize) {
    (void)u_sigmask;
    (void)sigsetsize;

    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    uint32_t nfds = static_cast<uint32_t>(nfds_val);
    if (nfds > MAX_POLL_FDS) return syscall::EINVAL;
    if (nfds > 0 && u_fds == 0) return syscall::EFAULT;

    size_t buf_size = nfds * sizeof(kernel_pollfd);
    kernel_pollfd* kfds = nullptr;
    if (nfds > 0) {
        kfds = static_cast<kernel_pollfd*>(heap::kzalloc(buf_size));
        if (!kfds) return syscall::ENOMEM;

        int32_t rc = mm::uaccess::copy_from_user(
            kfds, reinterpret_cast<const void*>(u_fds), buf_size);
        if (rc != mm::uaccess::OK) {
            heap::kfree(kfds);
            return syscall::EFAULT;
        }
    }

    uint64_t timeout_ns = 0;
    bool infinite = true;
    bool immediate = false;

    if (u_timeout != 0) {
        kernel_timespec ts;
        int32_t rc = mm::uaccess::copy_from_user(
            &ts, reinterpret_cast<const void*>(u_timeout), sizeof(ts));
        if (rc != mm::uaccess::OK) {
            heap::kfree(kfds);
            return syscall::EFAULT;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec > 999999999) {
            heap::kfree(kfds);
            return syscall::EINVAL;
        }
        timeout_ns = static_cast<uint64_t>(ts.tv_sec) * NS_PER_SEC
                   + static_cast<uint64_t>(ts.tv_nsec);
        infinite = false;
        immediate = (timeout_ns == 0);
    }

    int64_t result = do_poll(task, kfds, nfds, timeout_ns, infinite, immediate);
    if (result < 0) {
        heap::kfree(kfds);
        return result;
    }

    if (nfds > 0) {
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(u_fds), kfds, buf_size);
        heap::kfree(kfds);
        if (rc != mm::uaccess::OK) return syscall::EFAULT;
    }

    return result;
}

DEFINE_SYSCALL3(poll, u_fds, nfds_val, timeout_ms) {
    sched::task* task = sched::current();
    if (!task) return syscall::EIO;

    uint32_t nfds = static_cast<uint32_t>(nfds_val);
    if (nfds > MAX_POLL_FDS) return syscall::EINVAL;
    if (nfds > 0 && u_fds == 0) return syscall::EFAULT;

    size_t buf_size = nfds * sizeof(kernel_pollfd);
    kernel_pollfd* kfds = nullptr;
    if (nfds > 0) {
        kfds = static_cast<kernel_pollfd*>(heap::kzalloc(buf_size));
        if (!kfds) return syscall::ENOMEM;

        int32_t rc = mm::uaccess::copy_from_user(
            kfds, reinterpret_cast<const void*>(u_fds), buf_size);
        if (rc != mm::uaccess::OK) {
            heap::kfree(kfds);
            return syscall::EFAULT;
        }
    }

    int32_t ms = static_cast<int32_t>(timeout_ms);
    bool infinite = (ms < 0);
    bool immediate = (ms == 0);
    uint64_t timeout_ns = 0;
    if (ms > 0) {
        timeout_ns = static_cast<uint64_t>(ms) * NS_PER_MS;
    }

    int64_t result = do_poll(task, kfds, nfds, timeout_ns, infinite, immediate);
    if (result < 0) {
        heap::kfree(kfds);
        return result;
    }

    if (nfds > 0) {
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(u_fds), kfds, buf_size);
        heap::kfree(kfds);
        if (rc != mm::uaccess::OK) return syscall::EFAULT;
    }

    return result;
}
