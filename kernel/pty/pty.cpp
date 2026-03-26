#include "pty/pty.h"
#include "resource/resource.h"
#include "common/ring_buffer.h"
#include "fs/fstypes.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

namespace pty {

__PRIVILEGED_BSS static uint32_t g_next_pty_id;

__PRIVILEGED_CODE void pty_channel::ref_destroy(pty_channel* self) {
    if (!self) {
        return;
    }
    ring_buffer_destroy(self->m_input_rb);
    ring_buffer_destroy(self->m_output_rb);
    heap::kfree_delete(self);
}

__PRIVILEGED_CODE static void pty_echo_fn(void* ctx, const uint8_t* buf, size_t len) {
    auto* chan = static_cast<pty_channel*>(ctx);
    (void)ring_buffer_write(chan->m_output_rb, buf, len, true);
}

// Master ops

static ssize_t pty_master_read(
    resource::resource_object* obj, void* kdst, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !kdst) {
        return resource::ERR_INVAL;
    }
    auto* ep = static_cast<pty_endpoint*>(obj->impl);
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    ssize_t result;
    RUN_ELEVATED({
        result = ring_buffer_read(ep->channel->m_output_rb,
                                  static_cast<uint8_t*>(kdst), count, nonblock);
    });
    return result;
}

static ssize_t pty_master_write(
    resource::resource_object* obj, const void* ksrc, size_t count, uint32_t flags
) {
    (void)flags;
    if (!obj || !obj->impl || !ksrc) {
        return resource::ERR_INVAL;
    }
    auto* ep = static_cast<pty_endpoint*>(obj->impl);
    auto* chan = ep->channel.ptr();

    ssize_t result;
    RUN_ELEVATED({
        if (chan->m_input_rb->reader_closed) {
            result = resource::ERR_PIPE;
        } else {
            terminal::ld_input_buf(&chan->m_ld, chan->m_input_rb, &chan->m_echo,
                                   static_cast<const char*>(ksrc), count);
            result = static_cast<ssize_t>(count);
        }
    });
    return result;
}

static void pty_master_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }
    auto* ep = static_cast<pty_endpoint*>(obj->impl);

    RUN_ELEVATED({
        ring_buffer_close_write(ep->channel->m_input_rb);
        ring_buffer_close_read(ep->channel->m_output_rb);
        heap::kfree_delete(ep);
    });
    obj->impl = nullptr;
}

// Slave ops

static ssize_t pty_slave_read(
    resource::resource_object* obj, void* kdst, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !kdst) {
        return resource::ERR_INVAL;
    }
    auto* ep = static_cast<pty_endpoint*>(obj->impl);
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    ssize_t result;
    RUN_ELEVATED({
        result = ring_buffer_read(ep->channel->m_input_rb,
                                  static_cast<uint8_t*>(kdst), count, nonblock);
    });
    return result;
}

static ssize_t pty_slave_write_onlcr(pty_channel* chan,
                                      const uint8_t* src, size_t count,
                                      bool nonblock) {
    size_t consumed = 0;   // bytes consumed from src
    size_t i = 0;

    while (i < count) {
        // Find the next \n (or end of buffer)
        size_t chunk_start = i;
        while (i < count && src[i] != '\n') {
            i++;
        }

        // Write the chunk before the \n
        if (i > chunk_start) {
            size_t chunk_len = i - chunk_start;
            ssize_t n = ring_buffer_write(chan->m_output_rb,
                                          src + chunk_start,
                                          chunk_len, nonblock);
            if (n < 0) {
                return consumed > 0 ? static_cast<ssize_t>(consumed) : n;
            }
            consumed += static_cast<size_t>(n);
            // Short write: rewind i to retry remaining bytes next time
            if (static_cast<size_t>(n) < chunk_len) {
                return static_cast<ssize_t>(consumed);
            }
        }

        // If we hit a \n, write \r\n atomically (all-or-nothing).
        if (i < count && src[i] == '\n') {
            static const uint8_t crlf[2] = {'\r', '\n'};
            ssize_t n = ring_buffer_write_all(chan->m_output_rb,
                                              crlf, 2, nonblock);
            if (n < 0) {
                return consumed > 0 ? static_cast<ssize_t>(consumed) : n;
            }
            consumed++;  // count the original \n byte consumed
            i++;
        }
    }
    return static_cast<ssize_t>(consumed);
}

static ssize_t pty_slave_write(
    resource::resource_object* obj, const void* ksrc, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !ksrc) {
        return resource::ERR_INVAL;
    }
    auto* ep = static_cast<pty_endpoint*>(obj->impl);
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    auto* chan = ep->channel.ptr();
    ssize_t result;

    RUN_ELEVATED({
        if (chan->m_oflags & PTY_OFLAG_ONLCR) {
            result = pty_slave_write_onlcr(chan,
                         static_cast<const uint8_t*>(ksrc), count, nonblock);
        } else {
            result = ring_buffer_write(chan->m_output_rb,
                         static_cast<const uint8_t*>(ksrc), count, nonblock);
        }
    });
    return result;
}

static void pty_slave_close(resource::resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }
    auto* ep = static_cast<pty_endpoint*>(obj->impl);

    RUN_ELEVATED({
        ring_buffer_close_write(ep->channel->m_output_rb);
        ring_buffer_close_read(ep->channel->m_input_rb);
        heap::kfree_delete(ep);
    });
    obj->impl = nullptr;
}

static int32_t pty_slave_ioctl(
    resource::resource_object* obj, uint32_t cmd, uint64_t arg
) {
    (void)arg;
    if (!obj || !obj->impl) {
        return resource::ERR_INVAL;
    }
    auto* ep = static_cast<pty_endpoint*>(obj->impl);
    return terminal::ld_set_mode(&ep->channel->m_ld, cmd);
}

// Ops tables

static const resource::resource_ops g_pty_master_ops = {
    pty_master_read,
    pty_master_write,
    pty_master_close,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

static const resource::resource_ops g_pty_slave_ops = {
    pty_slave_read,
    pty_slave_write,
    pty_slave_close,
    pty_slave_ioctl,
    nullptr,
    nullptr,
    nullptr,
};

// Pair creation

__PRIVILEGED_CODE int32_t create_pair(
    resource::resource_object** out_master,
    resource::resource_object** out_slave
) {
    if (!out_master || !out_slave) {
        return resource::ERR_INVAL;
    }

    auto chan = rc::make_kref<pty_channel>();
    if (!chan) {
        return resource::ERR_NOMEM;
    }

    chan->m_input_rb = ring_buffer_create(PTY_RING_CAPACITY);
    if (!chan->m_input_rb) {
        return resource::ERR_NOMEM;
    }

    chan->m_output_rb = ring_buffer_create(PTY_RING_CAPACITY);
    if (!chan->m_output_rb) {
        ring_buffer_destroy(chan->m_input_rb);
        chan->m_input_rb = nullptr;
        return resource::ERR_NOMEM;
    }

    terminal::ld_init(&chan->m_ld);
    chan->m_echo = { pty_echo_fn, chan.ptr() };
    chan->m_id = __atomic_fetch_add(&g_next_pty_id, 1, __ATOMIC_RELAXED);
    chan->m_oflags = PTY_OFLAG_ONLCR;

    auto* ep_master = heap::kalloc_new<pty_endpoint>();
    if (!ep_master) {
        return resource::ERR_NOMEM;
    }
    ep_master->channel = chan;
    ep_master->is_master = true;

    auto* ep_slave = heap::kalloc_new<pty_endpoint>();
    if (!ep_slave) {
        heap::kfree_delete(ep_master);
        return resource::ERR_NOMEM;
    }
    ep_slave->channel = static_cast<rc::strong_ref<pty_channel>&&>(chan);
    ep_slave->is_master = false;

    auto* obj_master = heap::kalloc_new<resource::resource_object>();
    if (!obj_master) {
        heap::kfree_delete(ep_slave);
        heap::kfree_delete(ep_master);
        return resource::ERR_NOMEM;
    }
    obj_master->type = resource::resource_type::PTY;
    obj_master->ops = &g_pty_master_ops;
    obj_master->impl = ep_master;

    auto* obj_slave = heap::kalloc_new<resource::resource_object>();
    if (!obj_slave) {
        heap::kfree_delete(obj_master);
        heap::kfree_delete(ep_slave);
        heap::kfree_delete(ep_master);
        return resource::ERR_NOMEM;
    }
    obj_slave->type = resource::resource_type::PTY;
    obj_slave->ops = &g_pty_slave_ops;
    obj_slave->impl = ep_slave;

    *out_master = obj_master;
    *out_slave = obj_slave;
    return resource::OK;
}

} // namespace pty
