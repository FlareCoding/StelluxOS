#include "gpu/core.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "resource/resource.h"

namespace gpu {

__PRIVILEGED_CODE static rc::reaper::cleanup_result clean_up_queue(core_queue* q);

core_queue::core_queue(device& owner, context& in, uint32_t queue_kind) : dev(owner), ctx(in), kind(queue_kind) {
    cleanup_node.init(rc::reaper::reaper_thunk<core_queue, clean_up_queue, &core_queue::cleanup_node>);
}

__PRIVILEGED_CODE void core_queue::ref_destroy(core_queue* self) {
    heap::ufree_delete(self);
}

__PRIVILEGED_CODE void drop_queue(core_queue* q) {
    if (q->release()) {
        core_queue::ref_destroy(q);
    }
}

// Sets the destroying mark and reads the stopped one together under the queue's lock, so finish_stop() or the destroy
// that gets true defers the cleanup, never both
__PRIVILEGED_CODE static bool stop_work(core_queue& q, bool destroy) {
    bool marked = false;
    bool stopped = false;
    {
        sync::irq_lock_guard guard(q.lock);
        q.destroying = q.destroying || destroy;
        marked = !q.asked_to_stop;
        q.asked_to_stop = true;
        stopped = q.stopped;
    }

    if (marked) {
        q.dev.get_backend().stop_queue(q.backend_queue);
    }

    return stopped;
}

__PRIVILEGED_CODE void ask_to_stop(core_queue& q) {
    stop_work(q, false);
}

// The backend's reference drops last, after the queue's last touch of its context
__PRIVILEGED_CODE static void finish_stop(core_queue& q) {
    bool destroying = false;
    {
        sync::irq_lock_guard guard(q.lock);
        q.asked_to_stop = true;
        q.stopped = true;
        destroying = q.destroying;
    }

    if (destroying) {
        rc::reaper::defer(&q.cleanup_node);
    }

    drop_stop_count(q.ctx);
    drop_queue(&q);
}

__PRIVILEGED_CODE void queue_stopped(core_queue* q) {
    finish_stop(*q);
}

__PRIVILEGED_CODE static rc::reaper::cleanup_result clean_up_queue(core_queue* q) {
    context& c = q->ctx;
    q->dev.get_backend().destroy_queue(q->backend_queue);
    free_slot(c, q->index);
    drop_queue(q);
    drop_context(&c);

    return rc::reaper::DONE;
}

__PRIVILEGED_CODE static int32_t start_queue(device& dev, context& c, uint32_t kind, uint32_t* out_index) {
    auto* q = make<core_queue>(dev, c, kind);
    if (!q) {
        return fs::ERR_NOMEM;
    }

    sync::mutex_lock(c.lock);
    int32_t rc = reserve_slot(c, &q->index);
    if (rc == fs::OK) {
        rc = to_fs_error(dev.get_backend().create_queue(c.backend_space, kind, q, &q->backend_queue));
        if (rc != fs::OK) {
            unreserve_slot(c, q->index);
        }
    }

    if (rc != fs::OK) {
        sync::mutex_unlock(c.lock);
        heap::ufree_delete(q);
        return rc;
    }

    // The backend's reference, dropped once it reported the queue's stop
    q->add_ref();
    publish_queue(c, *q);
    sync::mutex_unlock(c.lock);

    // The call's own reference drops last, so a GPU_QUEUE_DESTROY of the published queue never frees it here
    *out_index = q->index;
    drop_queue(q);

    return fs::OK;
}

__PRIVILEGED_CODE static int32_t remove_queue(context& c, uint32_t index) {
    core_queue* q = take_queue_for_destroy(c, index);
    if (!q) {
        return fs::ERR_INVAL;
    }

    if (stop_work(*q, true)) {
        rc::reaper::defer(&q->cleanup_node);
    }

    drop_queue(q);

    return fs::OK;
}

__PRIVILEGED_CODE int32_t create_queue(device& dev, uint64_t arg) {
    gpu_queue_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK || args.padding != 0) {
        return fs::ERR_INVAL;
    }

    bool known_kind = args.kind == GPU_QUEUE_GRAPHICS || args.kind == GPU_QUEUE_COPY;
    if (!known_kind || (dev.info().queue_kinds & (1u << args.kind)) == 0) {
        return fs::ERR_INVAL;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = lookup_on_device(dev, args.context, resource::resource_type::GPU_CONTEXT, resource::RIGHT_WRITE, &obj);
    if (rc != fs::OK) {
        return rc;
    }

    // A queue whose index cannot be written out is destroyed again, since no call could name it
    auto& c = *static_cast<context*>(obj->impl);
    rc = start_queue(dev, c, args.kind, &args.queue);
    uint64_t user_index = arg + __builtin_offsetof(gpu_queue_args, queue);
    if (rc == fs::OK && copy_out(user_index, &args.queue, sizeof(args.queue)) != fs::OK) {
        remove_queue(c, args.queue);
        rc = fs::ERR_INVAL;
    }

    resource::resource_release(obj);

    return rc;
}

__PRIVILEGED_CODE int32_t destroy_queue(device& dev, uint64_t arg) {
    gpu_queue_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK || args.kind != 0 || args.padding != 0) {
        return fs::ERR_INVAL;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = lookup_on_device(dev, args.context, resource::resource_type::GPU_CONTEXT, resource::RIGHT_WRITE, &obj);
    if (rc != fs::OK) {
        return rc;
    }

    rc = remove_queue(*static_cast<context*>(obj->impl), args.queue);
    resource::resource_release(obj);

    return rc;
}

} // namespace gpu
