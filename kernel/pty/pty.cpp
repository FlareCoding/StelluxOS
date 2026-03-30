#include "pty/pty.h"
#include "resource/resource.h"
#include "common/ring_buffer.h"
#include "common/string.h"
#include "fs/fstypes.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "dynpriv/dynpriv.h"
#include "sync/poll.h"
#include "sync/wait_queue.h"
#include "terminal/terminal.h"
#include "terminal/termios.h"

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

/**
 * @brief Apply current termios settings to the line discipline.
 * Updates the LD mode (raw vs cooked) and output processing flags
 * based on the termios c_lflag and c_oflag fields.
 */
static void apply_termios_to_ld(pty_channel* chan) {
    // Determine line discipline mode from c_lflag
    bool canon = (chan->m_termios.c_lflag & terminal::ICANON) != 0;
    uint32_t new_ld_mode = canon ? 0 : terminal::LD_MODE_RAW;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(chan->m_ld.lock);
        if (chan->m_ld.mode != new_ld_mode) {
            chan->m_ld.line_len = 0;
            chan->m_ld.mode = new_ld_mode;
        }
        sync::spin_unlock_irqrestore(chan->m_ld.lock, irq);
    });

    // Update output processing flags from c_oflag
    if ((chan->m_termios.c_oflag & terminal::OPOST) &&
        (chan->m_termios.c_oflag & terminal::ONLCR)) {
        chan->m_oflags = PTY_OFLAG_ONLCR;
    } else {
        chan->m_oflags = 0;
    }

    // Update echo target: if ECHO is off, null the echo callback
    bool echo_on = (chan->m_termios.c_lflag & terminal::ECHO_F) != 0;
    if (echo_on) {
        chan->m_echo.write = pty_echo_fn;
        chan->m_echo.ctx = chan;
    } else {
        chan->m_echo.write = nullptr;
        chan->m_echo.ctx = nullptr;
    }
}

static int32_t pty_slave_ioctl(
    resource::resource_object* obj, uint32_t cmd, uint64_t arg
) {
    if (!obj || !obj->impl) {
        return resource::ERR_INVAL;
    }
    auto* ep = static_cast<pty_endpoint*>(obj->impl);
    auto* chan = ep->channel.ptr();

    switch (cmd) {
    case terminal::TCGETS: {
        // If arg == 0, treat as legacy STLX_TCSETS_RAW (backward compat)
        if (arg == 0) {
            return terminal::ld_set_mode(&chan->m_ld, terminal::STLX_TCSETS_RAW);
        }
        // Copy current termios to user space
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &chan->m_termios, sizeof(chan->m_termios));
        return (rc == mm::uaccess::OK) ? resource::OK : resource::ERR_INVAL;
    }

    case terminal::TCSETS:
    case terminal::TCSETSW:
    case terminal::TCSETSF: {
        // If arg == 0, treat as legacy STLX_TCSETS_COOKED (backward compat)
        if (arg == 0) {
            return terminal::ld_set_mode(&chan->m_ld, terminal::STLX_TCSETS_COOKED);
        }
        // Copy termios from user space
        terminal::kernel_termios new_termios;
        int32_t rc = mm::uaccess::copy_from_user(
            &new_termios, reinterpret_cast<const void*>(arg), sizeof(new_termios));
        if (rc != mm::uaccess::OK) {
            return resource::ERR_INVAL;
        }
        chan->m_termios = new_termios;
        apply_termios_to_ld(chan);
        return resource::OK;
    }

    case terminal::TIOCGWINSZ: {
        if (arg == 0) return resource::ERR_INVAL;
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &chan->m_winsize, sizeof(chan->m_winsize));
        return (rc == mm::uaccess::OK) ? resource::OK : resource::ERR_INVAL;
    }

    case terminal::TIOCSWINSZ: {
        if (arg == 0) return resource::ERR_INVAL;
        terminal::winsize ws;
        int32_t rc = mm::uaccess::copy_from_user(
            &ws, reinterpret_cast<const void*>(arg), sizeof(ws));
        if (rc != mm::uaccess::OK) return resource::ERR_INVAL;
        chan->m_winsize = ws;
        return resource::OK;
    }

    case terminal::TIOCGPGRP: {
        // Stub: return 0 (no process groups). This makes isatty() succeed
        // since isatty() is implemented as ioctl(fd, TIOCGPGRP, &pgrp)
        // and checks for != -1.
        if (arg == 0) return resource::OK;
        int32_t pgrp = 0;
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &pgrp, sizeof(pgrp));
        return (rc == mm::uaccess::OK) ? resource::OK : resource::ERR_INVAL;
    }

    default:
        return resource::ERR_UNSUP;
    }
}

static int32_t pty_master_ioctl(
    resource::resource_object* obj, uint32_t cmd, uint64_t arg
) {
    if (!obj || !obj->impl) {
        return resource::ERR_INVAL;
    }
    auto* ep = static_cast<pty_endpoint*>(obj->impl);
    auto* chan = ep->channel.ptr();

    switch (cmd) {
    case terminal::TIOCSWINSZ: {
        if (arg == 0) return resource::ERR_INVAL;
        terminal::winsize ws;
        int32_t rc = mm::uaccess::copy_from_user(
            &ws, reinterpret_cast<const void*>(arg), sizeof(ws));
        if (rc != mm::uaccess::OK) return resource::ERR_INVAL;
        chan->m_winsize = ws;
        return resource::OK;
    }

    case terminal::TIOCGWINSZ: {
        if (arg == 0) return resource::ERR_INVAL;
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &chan->m_winsize, sizeof(chan->m_winsize));
        return (rc == mm::uaccess::OK) ? resource::OK : resource::ERR_INVAL;
    }

    default:
        return resource::ERR_UNSUP;
    }
}

static uint32_t pty_master_poll(
    resource::resource_object* obj, sync::poll_table* pt
) {
    if (!obj || !obj->impl) return sync::POLL_NVAL;
    auto* ep = static_cast<pty_endpoint*>(obj->impl);
    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = ring_buffer_poll_read(ep->channel->m_output_rb, pt)
             | ring_buffer_poll_write(ep->channel->m_input_rb, pt);
    });
    return mask;
}

static uint32_t pty_slave_poll(
    resource::resource_object* obj, sync::poll_table* pt
) {
    if (!obj || !obj->impl) return sync::POLL_NVAL;
    auto* ep = static_cast<pty_endpoint*>(obj->impl);
    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = ring_buffer_poll_read(ep->channel->m_input_rb, pt)
             | ring_buffer_poll_write(ep->channel->m_output_rb, pt);
    });
    return mask;
}

// Ops tables

static const resource::resource_ops g_pty_master_ops = {
    pty_master_read,
    pty_master_write,
    pty_master_close,
    pty_master_ioctl,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    pty_master_poll,
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
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    pty_slave_poll,
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

    // Initialize termios to cooked defaults
    terminal::termios_init_default(&chan->m_termios);

    // Initialize winsize to a reasonable default (80x24)
    chan->m_winsize.ws_row = 24;
    chan->m_winsize.ws_col = 80;
    chan->m_winsize.ws_xpixel = 0;
    chan->m_winsize.ws_ypixel = 0;

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
