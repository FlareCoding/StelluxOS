#include "gpu/core.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "mm/vma.h"
#include "resource/resource.h"

namespace gpu {

__PRIVILEGED_CODE static rc::reaper::cleanup_result run_first_stage(context* c);
__PRIVILEGED_CODE static rc::reaper::cleanup_result run_second_stage(context* c);

context::context(device& owner) : dev(owner) {
    lock.init();
    first_stage.init(rc::reaper::reaper_thunk<context, run_first_stage, &context::first_stage>);
    second_stage.init(rc::reaper::reaper_thunk<context, run_second_stage, &context::second_stage>);
}

__PRIVILEGED_CODE void context::ref_destroy(context* self) {
    rc::reaper::defer(&self->first_stage);
}

__PRIVILEGED_CODE void drop_context(context* c) {
    if (c->release()) {
        context::ref_destroy(c);
    }
}

__PRIVILEGED_CODE void drop_stop_count(context& c) {
    if (c.stop_count.fetch_sub_acq_rel(1) == 1) {
        rc::reaper::defer(&c.second_stage);
    }
}

// Under the slot lock: references every queue a slot holds, a destroying one's included
__PRIVILEGED_CODE static uint32_t reference_queues(context& c, core_queue** out) {
    uint32_t count = 0;
    for (queue_slot& slot : c.slots) {
        if (slot.queue) {
            slot.queue->add_ref();
            out[count++] = slot.queue;
        }
    }

    return count;
}

__PRIVILEGED_CODE static void ask_each_to_stop(core_queue** queues, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        ask_to_stop(*queues[i]);
        drop_queue(queues[i]);
    }
}

__PRIVILEGED_CODE static rc::reaper::cleanup_result run_first_stage(context* c) {
    core_queue* queues[GPU_QUEUES_MAX];
    uint32_t count = 0;
    {
        sync::irq_lock_guard guard(c->slot_lock);
        count = reference_queues(*c, queues);
    }

    ask_each_to_stop(queues, count);
    drop_stop_count(*c);

    return rc::reaper::DONE;
}

// No call or callback reaches the context any more, and every queue stopped
__PRIVILEGED_CODE static rc::reaper::cleanup_result run_second_stage(context* c) {
    backend& b = c->dev.get_backend();
    for (queue_slot& slot : c->slots) {
        if (slot.queue) {
            b.destroy_queue(slot.queue->backend_queue);
        }
    }

    b.destroy_space(c->backend_space);

    for (queue_slot& slot : c->slots) {
        if (slot.queue) {
            drop_queue(slot.queue);
        }
    }

    while (mapping* m = c->mappings.min()) {
        c->mappings.remove(*m);
        drop_buffer(m->buf);
        heap::ufree_delete(m);
    }

    heap::ufree_delete(c);

    return rc::reaper::DONE;
}

__PRIVILEGED_CODE uint32_t lost_state(context& c, uint64_t* fault_va) {
    uint32_t reason = 0;
    {
        sync::irq_lock_guard guard(c.slot_lock);
        reason = c.lost_reason;
        *fault_va = c.fault_va;
    }

    if (reason == 0 && c.dev.failed()) {
        reason = GPU_LOST_DEVICE;
    }

    return reason;
}

__PRIVILEGED_CODE void lose_context(context& c, uint32_t reason, uint64_t fault_va) {
    core_queue* queues[GPU_QUEUES_MAX];
    uint32_t count = 0;
    {
        sync::irq_lock_guard guard(c.slot_lock);
        if (c.lost_reason != 0) {
            return;
        }

        c.lost_reason = reason;
        c.fault_va = fault_va;
        count = reference_queues(c, queues);
    }

    ask_each_to_stop(queues, count);
}

__PRIVILEGED_CODE void space_lost(context* c, uint32_t reason, uint64_t fault_va) {
    lose_context(*c, reason, fault_va);
}

__PRIVILEGED_CODE int32_t reserve_slot(context& c, uint32_t* out) {
    sync::irq_lock_guard guard(c.slot_lock);
    for (uint32_t i = 0; i < GPU_QUEUES_MAX; i++) {
        if (c.slots[i].state == slot_state::FREE) {
            c.slots[i].state = slot_state::RESERVED;
            c.stop_count.fetch_add_relaxed(1);
            *out = i;
            return fs::OK;
        }
    }

    return fs::ERR_BUSY;
}

__PRIVILEGED_CODE void unreserve_slot(context& c, uint32_t index) {
    {
        sync::irq_lock_guard guard(c.slot_lock);
        c.slots[index].state = slot_state::FREE;
    }

    drop_stop_count(c);
}

__PRIVILEGED_CODE bool publish_queue(context& c, core_queue& q) {
    sync::irq_lock_guard guard(c.slot_lock);
    q.add_ref();
    c.slots[q.index].queue = &q;
    c.slots[q.index].state = slot_state::PUBLISHED;

    return c.lost_reason != 0;
}

__PRIVILEGED_CODE core_queue* find_queue(context& c, uint32_t index) {
    if (index >= GPU_QUEUES_MAX) {
        return nullptr;
    }

    sync::irq_lock_guard guard(c.slot_lock);
    queue_slot& slot = c.slots[index];
    if (slot.state != slot_state::PUBLISHED) {
        return nullptr;
    }

    slot.queue->add_ref();

    return slot.queue;
}

__PRIVILEGED_CODE core_queue* take_queue_for_destroy(context& c, uint32_t index) {
    if (index >= GPU_QUEUES_MAX) {
        return nullptr;
    }

    sync::irq_lock_guard guard(c.slot_lock);
    queue_slot& slot = c.slots[index];
    if (slot.state != slot_state::PUBLISHED) {
        return nullptr;
    }

    slot.state = slot_state::DESTROYING;
    slot.queue->add_ref();
    c.add_ref();

    return slot.queue;
}

__PRIVILEGED_CODE void free_slot(context& c, uint32_t index) {
    sync::irq_lock_guard guard(c.slot_lock);
    c.slots[index] = {};
}

__PRIVILEGED_CODE static void context_close(resource::resource_object* obj) {
    drop_context(static_cast<context*>(obj->impl));
    obj->impl = nullptr;
}

static const resource::resource_ops g_context_ops = {
    .close = context_close,
};

__PRIVILEGED_CODE int32_t create_context(device& dev, uint64_t arg) {
    gpu_context_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK || args.lost != 0 || args.fault_va != 0) {
        return fs::ERR_INVAL;
    }

    if (dev.failed()) {
        return fs::ERR_IO;
    }

    auto* c = make<context>(dev);
    if (!c) {
        return fs::ERR_NOMEM;
    }

    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (!obj) {
        heap::ufree_delete(c);
        return fs::ERR_NOMEM;
    }

    int32_t rc = dev.get_backend().create_space(c, &c->backend_space);
    if (rc != OK) {
        heap::kfree_delete(obj);
        heap::ufree_delete(c);
        return to_fs_error(rc);
    }

    obj->type = resource::resource_type::GPU_CONTEXT;
    obj->ops = &g_context_ops;
    obj->impl = c;

    // From here the context's teardown destroys the space, a failed install's included
    uint64_t user_handle = arg + __builtin_offsetof(gpu_context_args, context);
    rc = install_handle(obj, resource::RIGHT_READ | resource::RIGHT_WRITE, user_handle);
    resource::resource_release(obj);

    return rc;
}

__PRIVILEGED_CODE int32_t query_context(device& dev, uint64_t arg) {
    gpu_context_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK) {
        return fs::ERR_INVAL;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = lookup_on_device(dev, args.context, resource::resource_type::GPU_CONTEXT, resource::RIGHT_READ, &obj);
    if (rc != fs::OK) {
        return rc;
    }

    args.lost = lost_state(*static_cast<context*>(obj->impl), &args.fault_va);
    resource::resource_release(obj);

    return copy_out(arg, &args, sizeof(args));
}

static bool has_user_range(const device& dev) {
    return dev.info().va_end != 0;
}

// Under the context's mutex: the first mapping that ends past `va`, or null
static mapping* first_ending_after(context& c, uint64_t va) {
    mapping probe = {};
    probe.va = va;

    mapping* after = c.mappings.upper_bound(probe);
    mapping* at_or_before = after ? c.mappings.prev(*after) : c.mappings.max();
    if (at_or_before && at_or_before->va + at_or_before->size > va) {
        return at_or_before;
    }

    return after;
}

static bool valid_map(const device& dev, const gpu_map_args& args, const buffer& buf) {
    if (!has_user_range(dev)) {
        return args.va == 0 && args.offset == 0 && args.size == 0 && args.kind == 0;
    }

    const gpu_info& info = dev.info();
    bool aligned = mm::is_page_aligned(args.va) && mm::is_page_aligned(args.offset) &&
                   mm::is_page_aligned(args.size);
    bool in_user_range = args.va >= info.va_start && args.va <= info.va_end && args.size <= info.va_end - args.va;
    bool in_buffer = args.offset <= buf.size() && args.size <= buf.size() - args.offset;

    return aligned && args.size != 0 && in_user_range && in_buffer;
}

// Under the context's mutex: no two mappings overlap
__PRIVILEGED_CODE static int32_t check_room(context& c, uint64_t va, uint64_t key_size) {
    mapping* next = first_ending_after(c, va);
    return next && next->va < va + key_size ? fs::ERR_BUSY : fs::OK;
}

__PRIVILEGED_CODE static int32_t add_mapping(context& c, buffer& buf, const gpu_map_args& args) {
    bool by_id = !has_user_range(c.dev);
    uint64_t va = by_id ? buf.device_id() : args.va;
    uint64_t size = by_id ? buf.size() : args.size;
    uint64_t key_size = by_id ? 1 : args.size;
    bool read_only = (args.flags & GPU_MAP_READ_ONLY) != 0;
    uint64_t fault_va = 0;

    sync::mutex_lock(c.lock);
    int32_t rc = lost_state(c, &fault_va) != 0 ? fs::ERR_IO : check_room(c, va, key_size);
    mapping* m = rc == fs::OK ? make<mapping>() : nullptr;
    if (rc == fs::OK && !m) {
        rc = fs::ERR_NOMEM;
    }

    if (rc == fs::OK) {
        memory* mem = buf.backend_memory();
        rc = to_fs_error(c.dev.get_backend().map(c.backend_space, va, mem, args.offset, size, args.kind, read_only));
    }

    if (rc == fs::OK) {
        buf.add_ref();
        m->va = va;
        m->size = key_size;
        m->buf = &buf;
        (void)c.mappings.insert(m);
    } else {
        heap::ufree_delete(m);
    }

    sync::mutex_unlock(c.lock);

    return rc;
}

__PRIVILEGED_CODE int32_t map_buffer(device& dev, uint64_t arg) {
    gpu_map_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK || (args.flags & ~GPU_MAP_READ_ONLY) != 0) {
        return fs::ERR_INVAL;
    }

    resource::resource_object* context_obj = nullptr;
    int32_t rc = lookup_on_device(
        dev,
        args.context,
        resource::resource_type::GPU_CONTEXT,
        resource::RIGHT_WRITE,
        &context_obj
    );
    if (rc != fs::OK) {
        return rc;
    }

    bool read_only = (args.flags & GPU_MAP_READ_ONLY) != 0;
    uint32_t rights = read_only ? resource::RIGHT_READ : resource::RIGHT_READ | resource::RIGHT_WRITE;
    resource::resource_object* buffer_obj = nullptr;
    rc = lookup_on_device(dev, args.buffer, resource::resource_type::GPU_BUFFER, rights, &buffer_obj);

    if (rc == fs::OK) {
        auto& buf = *static_cast<buffer*>(buffer_obj->impl);
        auto& c = *static_cast<context*>(context_obj->impl);
        rc = valid_map(dev, args, buf) ? add_mapping(c, buf, args) : fs::ERR_INVAL;
        resource::resource_release(buffer_obj);
    }

    resource::resource_release(context_obj);

    return rc;
}

__PRIVILEGED_CODE static unmap_marker* new_marker(uint32_t buffer_count) {
    void* block = allocate(sizeof(unmap_marker) + buffer_count * sizeof(buffer*));
    if (!block) {
        return nullptr;
    }

    auto* marker = new (block) unmap_marker();
    marker->count.store_relaxed(1);
    marker->buffers = reinterpret_cast<buffer**>(marker + 1);
    for (marker_join& join : marker->joins) {
        join.marker = marker;
    }

    return marker;
}

__PRIVILEGED_CODE void drop_marker(unmap_marker* marker) {
    if (marker->count.fetch_sub_acq_rel(1) != 1) {
        return;
    }

    for (uint32_t i = 0; i < marker->buffer_count; i++) {
        drop_buffer(marker->buffers[i]);
    }

    heap::ufree_delete(marker);
}

// Under the context's mutex: takes [start, end) out of the tree from `m` on, giving the marker a buffer reference per
// mapping cut. A mapping covered whole hands over its own, and `second` takes the part past `end` of one split in two
__PRIVILEGED_CODE static void cut_range(
    context& c,
    mapping* m,
    uint64_t start,
    uint64_t end,
    unmap_marker& marker,
    mapping* second
) {
    while (m && m->va < end) {
        mapping* next = c.mappings.next(*m);
        uint64_t m_end = m->va + m->size;
        marker.buffers[marker.buffer_count++] = m->buf;

        if (m->va >= start && m_end <= end) {
            c.mappings.remove(*m);
            heap::ufree_delete(m);
        } else if (m->va < start && m_end > end) {
            m->buf->add_ref();
            m->buf->add_ref();

            second->va = end;
            second->size = m_end - end;
            second->buf = m->buf;
            m->size = start - m->va;
            (void)c.mappings.insert(second);
        } else if (m->va < start) {
            m->buf->add_ref();
            m->size = start - m->va;
        } else {
            // Moving the start keeps the tree's order, since no other mapping overlaps the range
            uint64_t cut = end - m->va;
            m->buf->add_ref();
            m->va = end;
            m->size -= cut;
        }

        m = next;
    }
}

__PRIVILEGED_CODE static void join_queues(context& c, unmap_marker& marker) {
    core_queue* queues[GPU_QUEUES_MAX];
    uint32_t count = 0;
    {
        sync::irq_lock_guard guard(c.slot_lock);
        count = reference_queues(c, queues);
    }

    for (uint32_t i = 0; i < count; i++) {
        join_marker(*queues[i], marker.joins[i]);
        drop_queue(queues[i]);
    }
}

__PRIVILEGED_CODE static int32_t remove_range(context& c, uint64_t start, uint64_t end) {
    bool by_id = !has_user_range(c.dev);

    sync::mutex_lock(c.lock);
    mapping* first = first_ending_after(c, start);
    mapping* last = nullptr;
    uint32_t cut_count = 0;
    for (mapping* m = first; m && m->va < end; m = c.mappings.next(*m)) {
        last = m;
        cut_count++;
    }

    if (cut_count == 0) {
        sync::mutex_unlock(c.lock);
        if (by_id) {
            return fs::ERR_INVAL;
        }

        return c.dev.failed() ? fs::ERR_IO : fs::OK;
    }

    bool splits = first->va < start && first->va + first->size > end;
    mapping* second = splits ? make<mapping>() : nullptr;
    unmap_marker* marker = !splits || second ? new_marker(cut_count) : nullptr;
    if (!marker) {
        heap::ufree_delete(second);
        sync::mutex_unlock(c.lock);
        return fs::ERR_NOMEM;
    }

    uint64_t from = first->va > start ? first->va : start;
    uint64_t to = last->va + last->size < end ? last->va + last->size : end;
    uint64_t size = by_id ? first->buf->size() : to - from;

    // The range leaves the tree on ERR_IO too, since the failed backend keeps what the GPU may still translate
    int32_t rc = to_fs_error(c.dev.get_backend().unmap(c.backend_space, from, size));
    cut_range(c, first, start, end, *marker, second);
    sync::mutex_unlock(c.lock);

    join_queues(c, *marker);
    drop_marker(marker);

    return rc;
}

__PRIVILEGED_CODE int32_t unmap_range(device& dev, uint64_t arg) {
    gpu_map_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK) {
        return fs::ERR_INVAL;
    }

    const gpu_info& info = dev.info();
    bool aligned = mm::is_page_aligned(args.va) && mm::is_page_aligned(args.size);
    bool in_user_range = args.va >= info.va_start && args.va <= info.va_end && args.size <= info.va_end - args.va;
    bool valid = !has_user_range(dev) || (aligned && args.size != 0 && in_user_range);
    if (!valid) {
        return fs::ERR_INVAL;
    }

    resource::resource_object* obj = nullptr;
    int32_t rc = lookup_on_device(dev, args.context, resource::resource_type::GPU_CONTEXT, resource::RIGHT_WRITE, &obj);
    if (rc != fs::OK) {
        return rc;
    }

    uint64_t end = has_user_range(dev) ? args.va + args.size : args.va + 1;
    rc = remove_range(*static_cast<context*>(obj->impl), args.va, end);
    resource::resource_release(obj);

    return rc;
}

} // namespace gpu
