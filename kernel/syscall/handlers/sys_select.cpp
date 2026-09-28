#include "syscall/handlers/sys_select.h"

#include "sync/poll.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
#include "mm/uaccess.h"
#include "mm/heap.h"
#include "clock/clock.h"

constexpr size_t BITS_PER_LONG = sizeof(uint64_t) * 8;
constexpr size_t FD_SETSIZE_MAX = 1024;
constexpr int64_t NS_PER_SEC = 1000000000LL;
constexpr int64_t NS_PER_US  = 1000LL;

// Reported whether or not the caller asked for them
constexpr int16_t ALWAYS_EVENTS = static_cast<int16_t>(sync::POLL_ERR | sync::POLL_HUP | sync::POLL_NVAL);

struct select_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

static bool fd_is_set(const uint64_t* set, int fd) {
    return (set[fd / BITS_PER_LONG] >> (fd % BITS_PER_LONG)) & 1;
}

static void fd_set_bit(uint64_t* set, int fd) {
    set[fd / BITS_PER_LONG] |= (1ULL << (fd % BITS_PER_LONG));
}

// One readiness pass, keeping each object subscribed to `pt` in `held` so a close cannot free its queue
__PRIVILEGED_CODE static int64_t poll_round(sched::task* task, select_pollfd* pollfds, uint32_t npoll,
                                            sync::poll_table* pt, resource::resource_object** held) {
    int64_t ready = 0;

    for (uint32_t i = 0; i < npoll; i++) {
        resource::resource_object* obj = nullptr;
        int32_t rc = resource::get_handle_object(
            task->handles, static_cast<resource::handle_t>(pollfds[i].fd), 0, &obj);
        if (rc != resource::HANDLE_OK) {
            pollfds[i].revents = static_cast<int16_t>(sync::POLL_NVAL);
            ready++;
            continue;
        }

        if (!obj->ops || !obj->ops->poll) {
            pollfds[i].revents = static_cast<int16_t>(sync::POLL_NVAL);
            resource::resource_release(obj);
            ready++;
            continue;
        }

        uint32_t mask = obj->ops->poll(obj, pt);

        if (pt && held) {
            held[i] = obj;
        } else {
            resource::resource_release(obj);
        }

        pollfds[i].revents = static_cast<int16_t>(mask) & (pollfds[i].events | ALWAYS_EVENTS);

        if (pollfds[i].revents) {
            ready++;
        }
    }

    return ready;
}

// Unsubscribes before releasing the held objects
__PRIVILEGED_CODE static void finish_round(sync::poll_table& pt, resource::resource_object** held, uint32_t npoll) {
    sync::poll_cleanup(pt);

    for (uint32_t i = 0; i < npoll; i++) {
        resource::resource_release(held[i]);
        held[i] = nullptr;
    }
}

__PRIVILEGED_CODE static int64_t do_select(
    sched::task* task,
    int32_t nfds,
    uint64_t* kread, uint64_t* kwrite, uint64_t* kexcept,
    uint64_t timeout_ns, bool infinite, bool immediate
) {
    if (nfds <= 0 && !infinite && !immediate) {
        sync::poll_table pt;
        pt.init(task);

        sync::poll_wait(pt, timeout_ns);
        sync::poll_cleanup(pt);

        return signals::interrupt_pending(task) ? syscall::EINTR : 0;
    }

    size_t nwords = (static_cast<size_t>(nfds) + BITS_PER_LONG - 1) / BITS_PER_LONG;
    size_t max_pollfds = static_cast<size_t>(nfds);

    auto* pollfds = static_cast<select_pollfd*>(heap::uzalloc(max_pollfds * sizeof(select_pollfd)));
    if (!pollfds && max_pollfds > 0) {
        return syscall::ENOMEM;
    }

    auto* fdmap = static_cast<int32_t*>(heap::uzalloc(max_pollfds * sizeof(int32_t)));
    if (!fdmap && max_pollfds > 0) {
        heap::ufree(pollfds);
        return syscall::ENOMEM;
    }

    auto* held = static_cast<resource::resource_object**>(
        heap::uzalloc(max_pollfds * sizeof(resource::resource_object*)));
    if (!held && max_pollfds > 0) {
        heap::ufree(fdmap);
        heap::ufree(pollfds);
        return syscall::ENOMEM;
    }

    uint32_t npoll = 0;

    for (int32_t fd = 0; fd < nfds; fd++) {
        int16_t events = 0;

        if (kread && fd_is_set(kread, fd)) {
            events |= static_cast<int16_t>(sync::POLL_IN);
        }

        if (kwrite && fd_is_set(kwrite, fd)) {
            events |= static_cast<int16_t>(sync::POLL_OUT);
        }

        if (kexcept && fd_is_set(kexcept, fd)) {
            events |= static_cast<int16_t>(sync::POLL_PRI);
        }

        if (events) {
            pollfds[npoll].fd = fd;
            pollfds[npoll].events = events;
            pollfds[npoll].revents = 0;
            fdmap[npoll] = fd;
            npoll++;
        }
    }

    uint64_t deadline_ns = 0;
    if (!infinite && !immediate) {
        deadline_ns = clock::now_ns() + timeout_ns;
    }

    int64_t ready = 0;

    while (true) {
        sync::poll_table pt;
        pt.init(task);

        // An immediate probe polls without subscribing
        ready = poll_round(task, pollfds, npoll, immediate ? nullptr : &pt, held);
        if (pt.error.load_acquire()) {
            finish_round(pt, held, npoll);
            heap::ufree(held);
            heap::ufree(fdmap);
            heap::ufree(pollfds);
            return syscall::ENOMEM;
        }

        if (ready > 0 || immediate) {
            finish_round(pt, held, npoll);
            break;
        }

        uint64_t wait_ns = 0;
        if (!infinite) {
            uint64_t now = clock::now_ns();
            if (now >= deadline_ns) {
                finish_round(pt, held, npoll);
                break;
            }

            wait_ns = deadline_ns - now;
        }

        sync::poll_wait(pt, wait_ns);

        // A wake that left nothing ready is not a timeout, the next round waits again
        ready = poll_round(task, pollfds, npoll, nullptr, nullptr);
        finish_round(pt, held, npoll);

        if (ready > 0) {
            break;
        }

        if (signals::interrupt_pending(task)) {
            heap::ufree(held);
            heap::ufree(fdmap);
            heap::ufree(pollfds);
            return syscall::EINTR;
        }

        if (!infinite && clock::now_ns() >= deadline_ns) {
            break;
        }
    }

    if (kread) {
        for (size_t w = 0; w < nwords; w++) {
            kread[w] = 0;
        }
    }

    if (kwrite) {
        for (size_t w = 0; w < nwords; w++) {
            kwrite[w] = 0;
        }
    }

    if (kexcept) {
        for (size_t w = 0; w < nwords; w++) {
            kexcept[w] = 0;
        }
    }

    for (uint32_t i = 0; i < npoll; i++) {
        if (!pollfds[i].revents) {
            continue;
        }

        int32_t fd = fdmap[i];

        if (kread && (pollfds[i].revents & (sync::POLL_IN | sync::POLL_HUP | sync::POLL_ERR))) {
            fd_set_bit(kread, fd);
        }

        if (kwrite && (pollfds[i].revents & sync::POLL_OUT)) {
            fd_set_bit(kwrite, fd);
        }

        if (kexcept && (pollfds[i].revents & sync::POLL_PRI)) {
            fd_set_bit(kexcept, fd);
        }
    }

    heap::ufree(held);
    heap::ufree(fdmap);
    heap::ufree(pollfds);

    return ready;
}

// Linux select: (int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
DEFINE_SYSCALL5(select, nfds_val, u_readfds, u_writefds, u_exceptfds, u_timeout) {
    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    int32_t nfds = static_cast<int32_t>(nfds_val);
    if (nfds < 0 || static_cast<size_t>(nfds) > FD_SETSIZE_MAX) {
        return syscall::EINVAL;
    }

    size_t nwords = (static_cast<size_t>(nfds) + BITS_PER_LONG - 1) / BITS_PER_LONG;
    size_t set_bytes = nwords * sizeof(uint64_t);

    uint64_t* kread = nullptr;
    uint64_t* kwrite = nullptr;
    uint64_t* kexcept = nullptr;

    if (u_readfds) {
        kread = static_cast<uint64_t*>(heap::uzalloc(set_bytes));
        if (!kread) {
            return syscall::ENOMEM;
        }

        if (mm::uaccess::copy_from_user(kread, reinterpret_cast<const void*>(u_readfds), set_bytes) != mm::uaccess::OK) {
            heap::ufree(kread);
            return syscall::EFAULT;
        }
    }

    if (u_writefds) {
        kwrite = static_cast<uint64_t*>(heap::uzalloc(set_bytes));
        if (!kwrite) {
            heap::ufree(kread);
            return syscall::ENOMEM;
        }

        if (mm::uaccess::copy_from_user(kwrite, reinterpret_cast<const void*>(u_writefds), set_bytes) != mm::uaccess::OK) {
            heap::ufree(kwrite);
            heap::ufree(kread);
            return syscall::EFAULT;
        }
    }

    if (u_exceptfds) {
        kexcept = static_cast<uint64_t*>(heap::uzalloc(set_bytes));
        if (!kexcept) {
            heap::ufree(kwrite);
            heap::ufree(kread);
            return syscall::ENOMEM;
        }

        if (mm::uaccess::copy_from_user(kexcept, reinterpret_cast<const void*>(u_exceptfds), set_bytes) != mm::uaccess::OK) {
            heap::ufree(kexcept);
            heap::ufree(kwrite);
            heap::ufree(kread);
            return syscall::EFAULT;
        }
    }

    bool infinite = (u_timeout == 0);
    bool immediate = false;
    uint64_t timeout_ns = 0;

    if (u_timeout) {
        struct { int64_t tv_sec; int64_t tv_usec; } tv;
        if (mm::uaccess::copy_from_user(&tv, reinterpret_cast<const void*>(u_timeout), sizeof(tv)) != mm::uaccess::OK) {
            heap::ufree(kexcept);
            heap::ufree(kwrite);
            heap::ufree(kread);
            return syscall::EFAULT;
        }

        if (tv.tv_sec < 0 || tv.tv_usec < 0) {
            heap::ufree(kexcept);
            heap::ufree(kwrite);
            heap::ufree(kread);
            return syscall::EINVAL;
        }

        timeout_ns = static_cast<uint64_t>(tv.tv_sec) * NS_PER_SEC
                   + static_cast<uint64_t>(tv.tv_usec) * NS_PER_US;
        immediate = (timeout_ns == 0);
    }

    int64_t result = do_select(task, nfds, kread, kwrite, kexcept,
                               timeout_ns, infinite, immediate);

    if (result >= 0) {
        if (u_readfds &&
            mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_readfds), kread, set_bytes) != mm::uaccess::OK) {
            result = syscall::EFAULT;
        }

        if (result >= 0 && u_writefds &&
            mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_writefds), kwrite, set_bytes) != mm::uaccess::OK) {
            result = syscall::EFAULT;
        }

        if (result >= 0 && u_exceptfds &&
            mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_exceptfds), kexcept, set_bytes) != mm::uaccess::OK) {
            result = syscall::EFAULT;
        }
    }

    heap::ufree(kexcept);
    heap::ufree(kwrite);
    heap::ufree(kread);

    return result;
}

// Linux pselect6: (int nfds, fd_set*, fd_set*, fd_set*, struct timespec*, void *sigmask)
DEFINE_SYSCALL6(pselect6, nfds_val, u_readfds, u_writefds, u_exceptfds, u_timeout, u_sigmask) {
    (void)u_sigmask;

    sched::task* task = sched::current();
    if (!task) {
        return syscall::EIO;
    }

    int32_t nfds = static_cast<int32_t>(nfds_val);
    if (nfds < 0 || static_cast<size_t>(nfds) > FD_SETSIZE_MAX) {
        return syscall::EINVAL;
    }

    size_t nwords = (static_cast<size_t>(nfds) + BITS_PER_LONG - 1) / BITS_PER_LONG;
    size_t set_bytes = nwords * sizeof(uint64_t);

    uint64_t* kread = nullptr;
    uint64_t* kwrite = nullptr;
    uint64_t* kexcept = nullptr;

    if (u_readfds) {
        kread = static_cast<uint64_t*>(heap::uzalloc(set_bytes));
        if (!kread) {
            return syscall::ENOMEM;
        }

        if (mm::uaccess::copy_from_user(kread, reinterpret_cast<const void*>(u_readfds), set_bytes) != mm::uaccess::OK) {
            heap::ufree(kread);
            return syscall::EFAULT;
        }
    }

    if (u_writefds) {
        kwrite = static_cast<uint64_t*>(heap::uzalloc(set_bytes));
        if (!kwrite) {
            heap::ufree(kread);
            return syscall::ENOMEM;
        }

        if (mm::uaccess::copy_from_user(kwrite, reinterpret_cast<const void*>(u_writefds), set_bytes) != mm::uaccess::OK) {
            heap::ufree(kwrite);
            heap::ufree(kread);
            return syscall::EFAULT;
        }
    }

    if (u_exceptfds) {
        kexcept = static_cast<uint64_t*>(heap::uzalloc(set_bytes));
        if (!kexcept) {
            heap::ufree(kwrite);
            heap::ufree(kread);
            return syscall::ENOMEM;
        }

        if (mm::uaccess::copy_from_user(kexcept, reinterpret_cast<const void*>(u_exceptfds), set_bytes) != mm::uaccess::OK) {
            heap::ufree(kexcept);
            heap::ufree(kwrite);
            heap::ufree(kread);
            return syscall::EFAULT;
        }
    }

    bool infinite = (u_timeout == 0);
    bool immediate = false;
    uint64_t timeout_ns = 0;

    if (u_timeout) {
        struct { int64_t tv_sec; int64_t tv_nsec; } ts;
        if (mm::uaccess::copy_from_user(&ts, reinterpret_cast<const void*>(u_timeout), sizeof(ts)) != mm::uaccess::OK) {
            heap::ufree(kexcept);
            heap::ufree(kwrite);
            heap::ufree(kread);
            return syscall::EFAULT;
        }

        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= NS_PER_SEC) {
            heap::ufree(kexcept);
            heap::ufree(kwrite);
            heap::ufree(kread);
            return syscall::EINVAL;
        }

        timeout_ns = static_cast<uint64_t>(ts.tv_sec) * NS_PER_SEC
                   + static_cast<uint64_t>(ts.tv_nsec);
        immediate = (timeout_ns == 0);
    }

    int64_t result = do_select(task, nfds, kread, kwrite, kexcept,
                               timeout_ns, infinite, immediate);

    if (result >= 0) {
        if (u_readfds &&
            mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_readfds), kread, set_bytes) != mm::uaccess::OK) {
            result = syscall::EFAULT;
        }

        if (result >= 0 && u_writefds &&
            mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_writefds), kwrite, set_bytes) != mm::uaccess::OK) {
            result = syscall::EFAULT;
        }

        if (result >= 0 && u_exceptfds &&
            mm::uaccess::copy_to_user(reinterpret_cast<void*>(u_exceptfds), kexcept, set_bytes) != mm::uaccess::OK) {
            result = syscall::EFAULT;
        }
    }

    heap::ufree(kexcept);
    heap::ufree(kwrite);
    heap::ufree(kread);

    return result;
}
