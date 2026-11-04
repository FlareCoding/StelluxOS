#include "gpu/core.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "resource/resource.h"

namespace gpu {

constexpr uint64_t EVERY_SEQUENCE = ~static_cast<uint64_t>(0);

namespace {

// What a path ended under a queue's lock, finished once it holds no core lock
struct ended_work {
    collected_queues ready;
    submission_list  submissions;
    marker_list      markers;

    ended_work() {
        submissions.init();
        markers.init();
    }
};

} // namespace

static void on_deadline(timer::deadline_timer* t);
__PRIVILEGED_CODE static rc::reaper::cleanup_result clean_up_queue(core_queue* q);

core_queue::core_queue(device& owner, context& in, uint32_t queue_kind) : dev(owner), ctx(in), kind(queue_kind) {
    held.init();
    running.init();
    markers.init();
    timer::init_deadline_timer(&deadline, on_deadline);
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

__PRIVILEGED_CODE void collect(core_queue& q, collected_queues& ready) {
    // Pairs with the fence in run_collected(): a path that finds the mark set leaves its change to the holder, which
    // clears the mark before it reads the held head's waits
    sync::atomic_fence_seq_cst();
    if (q.collected.exchange_acq_rel(1) != 0) {
        drop_queue(&q);
        return;
    }

    q.collected_next = ready.first;
    ready.first = &q;
}

__PRIVILEGED_CODE static void free_submission(submission* sub) {
    for (uint32_t i = 0; i < sub->wait_count; i++) {
        drop_timeline(sub->waits[i].tl);
    }

    for (uint32_t i = 0; i < sub->signal_count; i++) {
        drop_timeline(sub->signals[i].tl);
    }

    heap::ufree_delete(sub);
}

__PRIVILEGED_CODE static void free_ended(submission_list& ended) {
    while (submission* sub = ended.pop_front()) {
        free_submission(sub);
    }
}

__PRIVILEGED_CODE static void drop_markers(marker_list& passed) {
    while (marker_join* join = passed.pop_front()) {
        drop_marker(join->marker);
    }
}

// Every function from here to run_collected() runs under the queue's lock. A reference it drops there is never the
// last, since its caller holds one

__PRIVILEGED_CODE static void end_submission(submission& sub, bool failed, ended_work& work) {
    for (uint32_t i = 0; i < sub.signal_count; i++) {
        reach_point(*sub.signals[i].tl, sub.signals[i].value, failed, work.ready);
    }

    work.submissions.push_back(&sub);
}

__PRIVILEGED_CODE static void start_deadline(core_queue& q) {
    q.deadline_ns = clock::now_ns() + q.dev.facts().submit_timeout_ns;

    // Each start holds a reference until its callback ends or a cancel removes the timer, and moving a scheduled
    // timer keeps the one it holds
    q.add_ref();
    if (timer::schedule(&q.deadline, q.deadline_ns)) {
        drop_queue(&q);
    }
}

__PRIVILEGED_CODE static void cancel_deadline(core_queue& q) {
    if (timer::cancel(&q.deadline) == timer::cancel_outcome::removed) {
        drop_queue(&q);
    }
}

__PRIVILEGED_CODE static void start_running(core_queue& q, submission& sub) {
    sub.sequence = ++q.last_run;
    q.running_bytes += sub.commands_size;
    if (q.running.empty()) {
        start_deadline(q);
    }

    q.running.push_back(&sub);
    q.dev.get_backend().run(q.backend_queue, sub.sequence, sub.commands, sub.commands_size);
}

__PRIVILEGED_CODE static void run_held(core_queue& q, ended_work& work) {
    while (!q.asked_to_stop) {
        submission* head = q.held.front();
        if (!head || head->unmet.load_acquire() != 0) {
            return;
        }

        bool failed = head->wait_failed.load_relaxed() != 0;
        bool fits = q.running_bytes + head->commands_size <= q.dev.facts().running_bytes_max;
        if (!failed && !fits) {
            return;
        }

        q.held.remove(head);
        if (failed) {
            end_submission(*head, true, work);
        } else {
            start_running(q, *head);
        }
    }
}

__PRIVILEGED_CODE static void end_held(core_queue& q, ended_work& work) {
    while (submission* sub = q.held.pop_front()) {
        for (uint32_t i = 0; i < sub->wait_count; i++) {
            unlink_wait(sub->waits[i]);
        }

        end_submission(*sub, true, work);
    }
}

__PRIVILEGED_CODE static void end_running(core_queue& q, uint64_t finished, bool failed, ended_work& work) {
    while (submission* sub = q.running.front()) {
        if (sub->sequence > finished) {
            return;
        }

        q.running.remove(sub);
        q.running_bytes -= sub->commands_size;
        end_submission(*sub, failed, work);
    }
}

__PRIVILEGED_CODE static void pass_markers(core_queue& q, uint64_t finished, ended_work& work) {
    while (marker_join* join = q.markers.front()) {
        if (join->sequence > finished) {
            return;
        }

        q.markers.remove(join);
        work.markers.push_back(join);
    }
}

__PRIVILEGED_CODE void run_collected(collected_queues& ready) {
    ended_work work;
    work.ready = ready;
    ready.first = nullptr;

    while (core_queue* q = work.ready.first) {
        work.ready.first = q->collected_next;
        {
            sync::irq_lock_guard guard(q->lock);
            q->collected.store_relaxed(0);
            sync::atomic_fence_seq_cst();
            run_held(*q, work);
        }

        drop_queue(q);
    }

    free_ended(work.submissions);
}

__PRIVILEGED_CODE static void finish(ended_work& work) {
    run_collected(work.ready);
    drop_markers(work.markers);
    free_ended(work.submissions);
}

// Sets the destroying mark and reads the stopped one together under the queue's lock, so finish_stop() or the destroy
// that gets true defers the cleanup, never both
__PRIVILEGED_CODE static bool stop_work(core_queue& q, bool destroy) {
    ended_work work;
    bool marked = false;
    bool stopped = false;
    {
        sync::irq_lock_guard guard(q.lock);
        q.destroying = q.destroying || destroy;
        marked = !q.asked_to_stop;
        q.asked_to_stop = true;
        end_held(q, work);
        stopped = q.stopped;
    }

    finish(work);

    if (marked && !q.dev.failed()) {
        q.dev.get_backend().stop_queue(q.backend_queue);
    }

    return stopped;
}

__PRIVILEGED_CODE void ask_to_stop(core_queue& q) {
    stop_work(q, false);
}

// The backend's reference drops last, after the queue's last touch of its context
__PRIVILEGED_CODE static void finish_stop(core_queue& q) {
    ended_work work;
    bool destroying = false;
    {
        sync::irq_lock_guard guard(q.lock);
        q.asked_to_stop = true;
        q.stopped = true;

        end_held(q, work);
        end_running(q, EVERY_SEQUENCE, true, work);
        pass_markers(q, EVERY_SEQUENCE, work);
        cancel_deadline(q);
        destroying = q.destroying;
    }

    finish(work);

    if (destroying) {
        rc::reaper::defer(&q.cleanup_node);
    }

    drop_stop_count(q.ctx);
    drop_queue(&q);
}

__PRIVILEGED_CODE void progress(core_queue* q, uint64_t finished) {
    ended_work work;
    {
        sync::irq_lock_guard guard(q->lock);
        uint64_t value = finished < q->last_run ? finished : q->last_run;
        if (value > q->last_finished) {
            q->last_finished = value;
            end_running(*q, value, false, work);
            if (q->running.empty()) {
                cancel_deadline(*q);
            } else {
                start_deadline(*q);
            }

            pass_markers(*q, value, work);
            run_held(*q, work);
        }
    }

    finish(work);
}

__PRIVILEGED_CODE void queue_stopped(core_queue* q) {
    q->dev.unlist_queue(*q);
    finish_stop(*q);
}

__PRIVILEGED_CODE void device_failed(device* d) {
    queue_list listed;
    listed.init();
    d->mark_failed(listed);

    while (core_queue* q = listed.pop_front()) {
        finish_stop(*q);
    }
}

__PRIVILEGED_CODE void join_marker(core_queue& q, marker_join& join) {
    sync::irq_lock_guard guard(q.lock);
    if (q.running.empty()) {
        return;
    }

    join.marker->count.fetch_add_relaxed(1);
    join.sequence = q.last_run;
    q.markers.push_back(&join);
}

__PRIVILEGED_CODE static void deadline_passed(core_queue& q) {
    context* lose = nullptr;
    bool stop = false;
    {
        sync::irq_lock_guard guard(q.lock);
        bool passed = !q.asked_to_stop && !q.running.empty() && clock::now_ns() >= q.deadline_ns;
        if (passed && q.ctx.try_add_ref()) {
            lose = &q.ctx;
        }

        stop = passed && !lose;
    }

    if (lose) {
        lose_context(*lose, GPU_LOST_HUNG, 0);
        drop_context(lose);
    }

    // A context whose teardown began is never lost, so the deadline stops the queue itself, as the first stage would
    if (stop) {
        ask_to_stop(q);
    }

    drop_queue(&q);
}

static void on_deadline(timer::deadline_timer* t) {
    core_queue* q = timer::owner_of<core_queue, &core_queue::deadline>(t);
    RUN_ELEVATED(deadline_passed(*q));
}

__PRIVILEGED_CODE static rc::reaper::cleanup_result clean_up_queue(core_queue* q) {
    context& c = q->ctx;
    q->dev.get_backend().destroy_queue(q->backend_queue);
    free_slot(c, q->index);
    drop_queue(q);
    drop_context(&c);

    return rc::reaper::DONE;
}

// A queue created after the GPU failed, which the backend never names, counts as stopped at once
__PRIVILEGED_CODE static void mark_stopped(core_queue& q) {
    sync::irq_lock_guard guard(q.lock);
    q.asked_to_stop = true;
    q.stopped = true;
}

__PRIVILEGED_CODE static int32_t start_queue(device& dev, context& c, uint32_t kind, uint32_t* out_index) {
    if (dev.failed()) {
        return fs::ERR_IO;
    }

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

    if (!dev.list_queue(*q)) {
        mark_stopped(*q);
        drop_stop_count(c);
    }

    bool lost = publish_queue(c, *q);
    sync::mutex_unlock(c.lock);

    lost = lost || dev.failed();
    if (lost) {
        ask_to_stop(*q);
    }

    // The call's own reference drops last, so a GPU_QUEUE_DESTROY of the published queue never frees it here
    *out_index = q->index;
    drop_queue(q);

    return lost ? fs::ERR_IO : fs::OK;
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

// copy_in() refuses a null address even for no bytes, and an empty list may name none
__PRIVILEGED_CODE static int32_t copy_list_in(void* dst, uint64_t src, size_t bytes) {
    return bytes == 0 ? fs::OK : copy_in(dst, src, bytes);
}

__PRIVILEGED_CODE static submission* new_submission(const gpu_submit_args& args) {
    size_t points_bytes = args.wait_count * sizeof(wait_entry) + args.signal_count * sizeof(signal_point);
    void* block = allocate(sizeof(submission) + points_bytes + args.commands_size);
    if (!block) {
        return nullptr;
    }

    auto* sub = new (block) submission();
    sub->waits = reinterpret_cast<wait_entry*>(sub + 1);
    sub->signals = reinterpret_cast<signal_point*>(sub->waits + args.wait_count);
    sub->commands = reinterpret_cast<uint8_t*>(sub->signals + args.signal_count);
    sub->commands_size = args.commands_size;

    return sub;
}

// A reference to the timeline `point` names, whose handle must hold `rights`
__PRIVILEGED_CODE static int32_t point_timeline(const gpu_point& point, uint32_t rights, timeline** out) {
    if (point.padding != 0) {
        return fs::ERR_INVAL;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = lookup_object(point.timeline, resource::resource_type::GPU_TIMELINE, rights, &obj);
    if (rc != fs::OK) {
        return rc;
    }

    *out = static_cast<timeline*>(obj->impl);
    (*out)->add_ref();
    resource::resource_release(obj);

    return fs::OK;
}

// Counts each point as it takes it, so free_submission() drops exactly the references taken
__PRIVILEGED_CODE static int32_t take_points(submission& sub, const gpu_submit_args& args) {
    gpu_point points[GPU_POINTS_MAX];
    if (copy_list_in(points, args.waits, args.wait_count * sizeof(gpu_point)) != fs::OK) {
        return fs::ERR_INVAL;
    }

    for (uint32_t i = 0; i < args.wait_count; i++) {
        wait_entry& entry = sub.waits[i];
        int32_t rc = point_timeline(points[i], resource::RIGHT_READ, &entry.tl);
        if (rc != fs::OK) {
            return rc;
        }

        entry.owner = &sub;
        entry.value = points[i].value;
        sub.wait_count++;
    }

    if (copy_list_in(points, args.signals, args.signal_count * sizeof(gpu_point)) != fs::OK) {
        return fs::ERR_INVAL;
    }

    for (uint32_t i = 0; i < args.signal_count; i++) {
        int32_t rc = point_timeline(points[i], resource::RIGHT_WRITE, &sub.signals[i].tl);
        if (rc != fs::OK) {
            return rc;
        }

        sub.signals[i].value = points[i].value;
        sub.signal_count++;
    }

    return fs::OK;
}

__PRIVILEGED_CODE static void hold(core_queue& q, submission& sub) {
    sub.queue = &q;
    sub.unmet.store_relaxed(sub.wait_count);
    q.held.push_back(&sub);

    for (uint32_t i = 0; i < sub.wait_count; i++) {
        if (link_wait(sub.waits[i])) {
            sub.unmet.fetch_sub_acq_rel(1);
        }
    }
}

__PRIVILEGED_CODE static int32_t queue_submission(context& c, uint32_t index, submission& sub) {
    // A loss marks the context before it asks each queue to stop, so a queue not yet asked must not take work
    uint64_t fault_va = 0;
    if (lost_state(c, &fault_va) != 0) {
        return fs::ERR_IO;
    }

    core_queue* q = find_queue(c, index);
    if (!q) {
        return fs::ERR_INVAL;
    }

    ended_work work;
    int32_t rc = fs::OK;
    {
        sync::irq_lock_guard guard(q->lock);
        if (q->asked_to_stop) {
            rc = q->destroying ? fs::ERR_INVAL : fs::ERR_IO;
        } else if (q->held.size() + q->running.size() >= GPU_QUEUE_PENDING_MAX) {
            rc = fs::ERR_BUSY;
        } else {
            hold(*q, sub);
            run_held(*q, work);
        }
    }

    finish(work);
    drop_queue(q);

    return rc;
}

__PRIVILEGED_CODE int32_t submit(device& dev, uint64_t arg) {
    gpu_submit_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK) {
        return fs::ERR_INVAL;
    }

    bool in_limits = args.commands_size <= GPU_COMMANDS_BYTES_MAX && args.wait_count <= GPU_POINTS_MAX &&
                     args.signal_count <= GPU_POINTS_MAX;
    if (!in_limits || args.padding != 0) {
        return fs::ERR_INVAL;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = lookup_on_device(dev, args.context, resource::resource_type::GPU_CONTEXT, resource::RIGHT_WRITE, &obj);
    if (rc != fs::OK) {
        return rc;
    }

    submission* sub = new_submission(args);
    rc = sub ? copy_list_in(sub->commands, args.commands, args.commands_size) : fs::ERR_NOMEM;
    if (rc == fs::OK) {
        rc = take_points(*sub, args);
    }

    if (rc == fs::OK) {
        rc = to_fs_error(dev.get_backend().check_commands(sub->commands, sub->commands_size));
    }

    if (rc == fs::OK) {
        rc = queue_submission(*static_cast<context*>(obj->impl), args.queue, *sub);
    }

    if (rc != fs::OK && sub) {
        free_submission(sub);
    }

    resource::resource_release(obj);

    return rc;
}

} // namespace gpu
