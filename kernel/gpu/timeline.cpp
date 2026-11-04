#include "gpu/core.h"
#include "clock/clock.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "signals/signal.h"
#include "sync/poll.h"

namespace gpu {

constexpr uint64_t NO_DEADLINE = ~static_cast<uint64_t>(0);

namespace {

// The points of one GPU_TIMELINE_WAIT, with a reference to each point's timeline
struct wait_set {
    gpu_point                  points[GPU_POINTS_MAX];
    resource::resource_object* held[GPU_POINTS_MAX];
    uint32_t                   count;
};

} // namespace

__PRIVILEGED_CODE void timeline::ref_destroy(timeline* self) {
    heap::ufree_delete(self);
}

__PRIVILEGED_CODE static uint64_t read_value(timeline& tl) {
    sync::irq_lock_guard guard(tl.lock);
    return tl.value;
}

__PRIVILEGED_CODE static void timeline_close(resource::resource_object* obj) {
    auto* tl = static_cast<timeline*>(obj->impl);
    if (tl->release()) {
        timeline::ref_destroy(tl);
    }

    obj->impl = nullptr;
}

static const resource::resource_ops g_timeline_ops = {
    .close = timeline_close,
};

__PRIVILEGED_CODE static int32_t copy_args_in(gpu_timeline_args& args, uint64_t arg) {
    int32_t rc = copy_in(&args, arg, sizeof(args));
    return rc == fs::OK && args.flags != 0 ? fs::ERR_INVAL : rc;
}

__PRIVILEGED_CODE int32_t create_timeline(uint64_t arg) {
    gpu_timeline_args args = {};
    int32_t rc = copy_args_in(args, arg);
    if (rc != fs::OK) {
        return rc;
    }

    auto* tl = heap::ualloc_new<timeline>();
    if (!tl) {
        return fs::ERR_NOMEM;
    }

    tl->lock = sync::SPINLOCK_INIT;
    tl->value = args.value;
    tl->waiters.init();

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        heap::ufree_delete(tl);
        return fs::ERR_NOMEM;
    }

    obj->type = resource::resource_type::GPU_TIMELINE;
    obj->ops = &g_timeline_ops;
    obj->impl = tl;

    uint64_t user_handle = arg + __builtin_offsetof(gpu_timeline_args, timeline);
    rc = install_handle(obj, resource::RIGHT_READ | resource::RIGHT_WRITE, user_handle);
    resource::resource_release(obj);

    return rc;
}

__PRIVILEGED_CODE int32_t signal_timeline(uint64_t arg) {
    gpu_timeline_args args = {};
    int32_t rc = copy_args_in(args, arg);
    if (rc != fs::OK) {
        return rc;
    }

    resource::resource_object* obj = nullptr;
    rc = lookup_object(args.timeline, resource::resource_type::GPU_TIMELINE, resource::RIGHT_WRITE, &obj);
    if (rc != fs::OK) {
        return rc;
    }

    auto* tl = static_cast<timeline*>(obj->impl);
    bool rises = false;
    {
        sync::irq_lock_guard guard(tl->lock);
        rises = args.value > tl->value;
        if (rises) {
            tl->value = args.value;
        }
    }

    if (rises) {
        sync::wake_all(tl->waiters);
    }

    resource::resource_release(obj);

    return rises ? fs::OK : fs::ERR_INVAL;
}

__PRIVILEGED_CODE int32_t query_timeline(uint64_t arg) {
    gpu_timeline_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK) {
        return fs::ERR_INVAL;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = lookup_object(args.timeline, resource::resource_type::GPU_TIMELINE, resource::RIGHT_READ, &obj);
    if (rc != fs::OK) {
        return rc;
    }

    args.flags = 0;
    args.value = read_value(*static_cast<timeline*>(obj->impl));
    resource::resource_release(obj);

    return copy_out(arg, &args, sizeof(args));
}

__PRIVILEGED_CODE static void release_held(wait_set& set, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        resource::resource_release(set.held[i]);
    }
}

// Holds no reference when it fails, so its caller releases none
__PRIVILEGED_CODE static int32_t take_points(wait_set& set, const gpu_wait_args& args) {
    if (copy_in(set.points, args.points, args.count * sizeof(gpu_point)) != fs::OK) {
        return fs::ERR_INVAL;
    }

    for (uint32_t i = 0; i < args.count; i++) {
        if (set.points[i].padding != 0) {
            release_held(set, i);
            return fs::ERR_INVAL;
        }

        int32_t rc = lookup_object(
            set.points[i].timeline,
            resource::resource_type::GPU_TIMELINE,
            resource::RIGHT_READ,
            &set.held[i]
        );
        if (rc != fs::OK) {
            release_held(set, i);
            return rc;
        }
    }

    set.count = args.count;

    return fs::OK;
}

__PRIVILEGED_CODE static bool satisfied(const wait_set& set, bool want_all, uint32_t* reached) {
    for (uint32_t i = 0; i < set.count; i++) {
        bool at_point = read_value(*static_cast<timeline*>(set.held[i]->impl)) >= set.points[i].value;
        if (!want_all && at_point) {
            *reached = i;
            return true;
        }

        if (want_all && !at_point) {
            return false;
        }
    }

    return want_all;
}

__PRIVILEGED_CODE static int32_t wait_on(const wait_set& set, bool want_all, uint64_t deadline_ns, uint32_t* reached) {
    sched::task* task = sched::current();

    while (true) {
        // Each sleep follows a check made while subscribed, so a rise after the check still ends the sleep
        sync::poll_table pt;
        pt.init(task);
        for (uint32_t i = 0; i < set.count; i++) {
            sync::poll_subscribe(pt, static_cast<timeline*>(set.held[i]->impl)->waiters);
        }

        int32_t rc = fs::OK;
        bool slept = false;
        uint64_t now = clock::now_ns();
        if (satisfied(set, want_all, reached)) {
            rc = fs::OK;
        } else if (deadline_ns != NO_DEADLINE && now >= deadline_ns) {
            rc = fs::ERR_TIMEDOUT;
        } else if (pt.error.load_acquire()) {
            rc = fs::ERR_NOMEM;
        } else if (signals::interrupt_pending(task)) {
            rc = fs::ERR_INTR;
        } else {
            uint64_t sleep_ns = deadline_ns == NO_DEADLINE ? 0 : deadline_ns - now;
            sync::poll_wait(pt, sleep_ns);
            slept = true;
        }

        sync::poll_cleanup(pt);

        if (!slept) {
            return rc;
        }
    }
}

__PRIVILEGED_CODE int32_t wait_timelines(uint64_t arg) {
    gpu_wait_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK) {
        return fs::ERR_INVAL;
    }

    bool count_in_range = args.count >= 1 && args.count <= GPU_POINTS_MAX;
    bool known_flags = (args.flags & ~GPU_WAIT_ALL) == 0;
    if (!count_in_range || !known_flags || args.padding != 0) {
        return fs::ERR_INVAL;
    }

    wait_set set = {};
    int32_t rc = take_points(set, args);
    if (rc != fs::OK) {
        return rc;
    }

    bool want_all = (args.flags & GPU_WAIT_ALL) != 0;
    uint32_t reached = 0;
    rc = wait_on(set, want_all, args.deadline_ns, &reached);
    release_held(set, set.count);

    if (rc != fs::OK || want_all) {
        return rc;
    }

    return copy_out(arg + __builtin_offsetof(gpu_wait_args, reached), &reached, sizeof(reached));
}

} // namespace gpu
