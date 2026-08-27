#include "pty/pty.h"
#include "resource/resource.h"
#include "common/ring_buffer.h"
#include "fs/fstypes.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "dynpriv/dynpriv.h"
#include "signals/signal.h"
#include "sync/poll.h"
#include "sync/wait_queue.h"
#include "terminal/terminal.h"

namespace pty {

constexpr uint32_t TCGETS      = 0x5401;
constexpr uint32_t TCSETS      = 0x5402;
constexpr uint32_t TCSETSW     = 0x5403;
constexpr uint32_t TCSETSF     = 0x5404;
constexpr uint32_t TIOCGWINSZ  = 0x5413;
constexpr uint32_t TIOCSWINSZ  = 0x5414;

constexpr uint32_t LINUX_ISIG   = 0x0001;
constexpr uint32_t LINUX_ECHO   = 0x0008;
constexpr uint32_t LINUX_ICANON = 0x0002;

struct linux_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};

__PRIVILEGED_BSS static sync::atomic<uint32_t> g_next_pty_id;

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

__PRIVILEGED_CODE static void pty_signal_fn(void* ctx, uint32_t sig) {
    auto* chan = static_cast<pty_channel*>(ctx);
    uint32_t fg = chan->m_fg_group.load_acquire();
    if (fg) {
        (void)signals::send_to_group_id(fg, sig);
    }
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
                                   &chan->m_sig,
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
    size_t consumed = 0; // bytes consumed from src
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

            // Partial ring write, report progress and let the caller retry the rest
            if (static_cast<size_t>(n) < chunk_len) {
                return static_cast<ssize_t>(consumed);
            }
        }

        // Write \r\n all-or-nothing so a short write cannot split the pair
        if (i < count && src[i] == '\n') {
            static const uint8_t crlf[2] = {'\r', '\n'};
            ssize_t n = ring_buffer_write_all(chan->m_output_rb,
                                              crlf, 2, nonblock);
            if (n < 0) {
                return consumed > 0 ? static_cast<ssize_t>(consumed) : n;
            }

            consumed++; // count the original \n byte consumed
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

static int32_t do_tcgets(pty_channel* chan, uint64_t arg) {
    linux_termios t = {};

    if (chan->m_ld.mode != terminal::LD_MODE_RAW) {
        t.c_lflag = LINUX_ICANON | LINUX_ECHO;
    }
    if (chan->m_ld.isig) {
        t.c_lflag |= LINUX_ISIG;
    }

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(arg), &t, sizeof(t));

    return (rc == mm::uaccess::OK) ? resource::OK : resource::ERR_INVAL;
}

static int32_t do_tcsets(pty_channel* chan, uint64_t arg) {
    linux_termios t = {};
    int32_t rc = mm::uaccess::copy_from_user(
        &t, reinterpret_cast<const void*>(arg), sizeof(t));
    
    if (rc != mm::uaccess::OK) {
        return resource::ERR_INVAL;
    }

    uint32_t mode = (t.c_lflag & LINUX_ICANON)
        ? terminal::STLX_TCSETS_COOKED
        : terminal::STLX_TCSETS_RAW;

    // The mode shortcut pairs ISIG with it, the termios bit then decides
    terminal::ld_set_mode(&chan->m_ld, mode);
    terminal::ld_set_isig(&chan->m_ld, (t.c_lflag & LINUX_ISIG) != 0);
    return resource::OK;
}

static int32_t do_tiocgpgrp(pty_channel* chan, uint64_t arg) {
    int32_t g = static_cast<int32_t>(chan->m_fg_group.load_acquire());

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(arg), &g, sizeof(g));

    return (rc == mm::uaccess::OK) ? resource::OK : resource::ERR_INVAL;
}

static int32_t do_tiocspgrp(pty_channel* chan, uint64_t arg) {
    int32_t g = 0;
    int32_t rc = mm::uaccess::copy_from_user(
        &g, reinterpret_cast<const void*>(arg), sizeof(g));
    if (rc != mm::uaccess::OK || g < 0) {
        return resource::ERR_INVAL;
    }

    // POSIX requires an existing process group, 0 clears the foreground
    if (g > 0 &&
        signals::send_to_group_id(static_cast<uint32_t>(g), 0) != signals::OK) {
        return resource::ERR_INVAL;
    }

    chan->m_fg_group.store_release(static_cast<uint32_t>(g));
    return resource::OK;
}

static int32_t do_tiocgwinsz(pty_channel* chan, uint64_t arg) {
    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(arg), &chan->m_winsize, sizeof(chan->m_winsize));

    return (rc == mm::uaccess::OK) ? resource::OK : resource::ERR_INVAL;
}

static int32_t do_tiocswinsz(pty_channel* chan, uint64_t arg) {
    pty_winsize w = {};
    int32_t rc = mm::uaccess::copy_from_user(
        &w, reinterpret_cast<const void*>(arg), sizeof(w));
    if (rc != mm::uaccess::OK) {
        return resource::ERR_INVAL;
    }

    chan->m_winsize = w;
    return resource::OK;
}

static int32_t pty_termios_ioctl(pty_channel* chan, uint32_t cmd, uint64_t arg) {
    switch (cmd) {
        case TCGETS:                       return do_tcgets(chan, arg);
        case TCSETS:
        case TCSETSW:
        case TCSETSF:                      return do_tcsets(chan, arg);
        case TIOCGWINSZ:                   return do_tiocgwinsz(chan, arg);
        case TIOCSWINSZ:                   return do_tiocswinsz(chan, arg);
        case terminal::TIOCGPGRP:          return do_tiocgpgrp(chan, arg);
        case terminal::TIOCSPGRP:          return do_tiocspgrp(chan, arg);
        case terminal::STLX_TCSETS_RAW:
        case terminal::STLX_TCSETS_COOKED: return terminal::ld_set_mode(&chan->m_ld, cmd);
        default:                           return resource::ERR_INVAL;
    }
}

// Termios and winsize state is channel-level, shared by both endpoints
static int32_t pty_ioctl(
    resource::resource_object* obj, uint32_t cmd, uint64_t arg
) {
    if (!obj || !obj->impl) {
        return resource::ERR_INVAL;
    }

    auto* ep = static_cast<pty_endpoint*>(obj->impl);
    return pty_termios_ioctl(ep->channel.ptr(), cmd, arg);
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
    pty_ioctl,
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
    pty_ioctl,
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
    chan->m_sig = { pty_signal_fn, chan.ptr() };

    chan->m_id = g_next_pty_id.fetch_add_relaxed(1);
    chan->m_oflags = PTY_OFLAG_ONLCR;
    chan->m_fg_group.store_relaxed(0);
    chan->m_winsize = { 24, 80, 0, 0 };

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
