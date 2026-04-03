#include "pipe/pipe.h"
#include "resource/resource.h"
#include "common/ring_buffer.h"
#include "fs/fstypes.h"
#include "mm/heap.h"
#include "sync/poll.h"
#include "dynpriv/dynpriv.h"

namespace pipe {

__PRIVILEGED_CODE void pipe_channel::ref_destroy(pipe_channel* self) {
    if (!self) {
        return;
    }
    ring_buffer_destroy(self->rb);
    heap::kfree_delete(self);
}

// Read end ops

static ssize_t pipe_read(
    resource::resource_object* obj, void* kdst, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !kdst) {
        return resource::ERR_INVAL;
    }
    auto* ep = static_cast<pipe_endpoint*>(obj->impl);
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    ssize_t result;
    RUN_ELEVATED({
        result = ring_buffer_read(ep->channel->rb,
                                  static_cast<uint8_t*>(kdst), count, nonblock);
    });
    return result;
}

static void pipe_read_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }
    auto* ep = static_cast<pipe_endpoint*>(obj->impl);
    RUN_ELEVATED({
        ring_buffer_close_read(ep->channel->rb);
        heap::kfree_delete(ep);
    });
    obj->impl = nullptr;
}

static uint32_t pipe_read_poll(
    resource::resource_object* obj, sync::poll_table* pt
) {
    if (!obj || !obj->impl) {
        return sync::POLL_NVAL;
    }
    auto* ep = static_cast<pipe_endpoint*>(obj->impl);
    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = ring_buffer_poll_read(ep->channel->rb, pt);
    });
    return mask;
}

// Write end ops

static ssize_t pipe_write(
    resource::resource_object* obj, const void* ksrc, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !ksrc) {
        return resource::ERR_INVAL;
    }
    auto* ep = static_cast<pipe_endpoint*>(obj->impl);
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    ssize_t result;
    RUN_ELEVATED({
        result = ring_buffer_write(ep->channel->rb,
                                   static_cast<const uint8_t*>(ksrc), count, nonblock);
    });
    return result;
}

static void pipe_write_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }
    auto* ep = static_cast<pipe_endpoint*>(obj->impl);
    RUN_ELEVATED({
        ring_buffer_close_write(ep->channel->rb);
        heap::kfree_delete(ep);
    });
    obj->impl = nullptr;
}

static uint32_t pipe_write_poll(
    resource::resource_object* obj, sync::poll_table* pt
) {
    if (!obj || !obj->impl) {
        return sync::POLL_NVAL;
    }
    auto* ep = static_cast<pipe_endpoint*>(obj->impl);
    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = ring_buffer_poll_write(ep->channel->rb, pt);
    });
    return mask;
}

// Ops tables

static const resource::resource_ops g_pipe_read_ops = {
    pipe_read,       // read
    nullptr,         // write
    pipe_read_close, // close
    nullptr,         // ioctl
    nullptr,         // mmap
    nullptr,         // sendto
    nullptr,         // recvfrom
    nullptr,         // bind
    nullptr,         // listen
    nullptr,         // accept
    nullptr,         // connect
    nullptr,         // setsockopt
    nullptr,         // getsockopt
    pipe_read_poll,  // poll
    nullptr,         // shutdown
};

static const resource::resource_ops g_pipe_write_ops = {
    nullptr,          // read
    pipe_write,       // write
    pipe_write_close, // close
    nullptr,          // ioctl
    nullptr,          // mmap
    nullptr,          // sendto
    nullptr,          // recvfrom
    nullptr,          // bind
    nullptr,          // listen
    nullptr,          // accept
    nullptr,          // connect
    nullptr,          // setsockopt
    nullptr,          // getsockopt
    pipe_write_poll,  // poll
    nullptr,          // shutdown
};

// Pair creation

__PRIVILEGED_CODE int32_t create_pair(
    resource::resource_object** out_read,
    resource::resource_object** out_write
) {
    if (!out_read || !out_write) {
        return resource::ERR_INVAL;
    }

    auto chan = rc::make_kref<pipe_channel>();
    if (!chan) {
        return resource::ERR_NOMEM;
    }

    chan->rb = ring_buffer_create(PIPE_RING_CAPACITY);
    if (!chan->rb) {
        return resource::ERR_NOMEM;
    }

    auto* ep_read = heap::kalloc_new<pipe_endpoint>();
    if (!ep_read) {
        return resource::ERR_NOMEM;
    }
    ep_read->channel = chan;
    ep_read->is_read_end = true;

    auto* ep_write = heap::kalloc_new<pipe_endpoint>();
    if (!ep_write) {
        heap::kfree_delete(ep_read);
        return resource::ERR_NOMEM;
    }
    ep_write->channel = static_cast<rc::strong_ref<pipe_channel>&&>(chan);
    ep_write->is_read_end = false;

    auto* obj_read = heap::kalloc_new<resource::resource_object>();
    if (!obj_read) {
        heap::kfree_delete(ep_write);
        heap::kfree_delete(ep_read);
        return resource::ERR_NOMEM;
    }
    obj_read->type = resource::resource_type::PIPE;
    obj_read->ops = &g_pipe_read_ops;
    obj_read->impl = ep_read;

    auto* obj_write = heap::kalloc_new<resource::resource_object>();
    if (!obj_write) {
        heap::kfree_delete(obj_read);
        heap::kfree_delete(ep_write);
        heap::kfree_delete(ep_read);
        return resource::ERR_NOMEM;
    }
    obj_write->type = resource::resource_type::PIPE;
    obj_write->ops = &g_pipe_write_ops;
    obj_write->impl = ep_write;

    *out_read = obj_read;
    *out_write = obj_write;
    return resource::OK;
}

} // namespace pipe
