#include "terminal/terminal.h"
#include "terminal/console_node.h"
#include "terminal/line_discipline.h"
#include "common/ring_buffer.h"
#include "serial/serial.h"
#include "resource/resource.h"
#include "signals/signal.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "fs/fs.h"
#include "fs/fstypes.h"
#include "fs/devfs/devfs.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "sync/atomic.h"
#include "sync/poll.h"

namespace terminal {

constexpr size_t INPUT_RING_CAPACITY = 4096;

__PRIVILEGED_BSS static struct {
    ring_buffer* input_rb;
    line_discipline ld;
    sync::atomic<uint32_t> fg_group; // foreground process group, 0 = none
} g_console;

__PRIVILEGED_CODE static void serial_echo(void* ctx, const uint8_t* buf, size_t len) {
    (void)ctx;
    serial::write(reinterpret_cast<const char*>(buf), len);
}

__PRIVILEGED_DATA static const echo_target g_serial_echo = {
    serial_echo,
    nullptr,
};

__PRIVILEGED_CODE static void console_signal_fn(void* ctx, uint32_t sig) {
    (void)ctx;
    uint32_t fg = g_console.fg_group.load_acquire();
    if (fg) {
        (void)signals::send_to_group_id(fg, sig);
    }
}

__PRIVILEGED_DATA static const signal_target g_console_sig = {
    console_signal_fn,
    nullptr,
};

__PRIVILEGED_CODE int32_t init() {
    g_console.input_rb = ring_buffer_create(INPUT_RING_CAPACITY);
    if (!g_console.input_rb) {
        log::error("terminal: failed to create ring buffer");
        return ERR;
    }

    ld_init(&g_console.ld);

    serial::set_rx_callback(input_char);
    if (serial::enable_rx_interrupt() != serial::OK) {
        log::warn("terminal: serial RX interrupt setup failed");
    }

    void* cn_mem = heap::kzalloc(sizeof(console_node));
    if (!cn_mem) {
        log::error("terminal: failed to allocate console_node");
        return ERR;
    }

    auto* cn = new (cn_mem) console_node(nullptr, "console");
    int32_t rc = devfs::add_char_device("console", cn);
    if (rc != devfs::OK) {
        log::error("terminal: failed to register /dev/console");
        return ERR;
    }

    log::info("terminal: console initialized (cooked mode, /dev/console registered)");
    return OK;
}

__PRIVILEGED_CODE void input_char(char c) {
    ld_input(&g_console.ld, g_console.input_rb, &g_serial_echo,
             &g_console_sig, c);
}

__PRIVILEGED_CODE ring_buffer* console_input_rb() {
    return g_console.input_rb;
}

__PRIVILEGED_CODE static ssize_t terminal_read(
    resource::resource_object* obj, void* kdst, size_t count, uint32_t flags
) {
    if (!obj || !obj->impl || !kdst) {
        return RB_ERR_INVAL;
    }

    auto* rb = static_cast<ring_buffer*>(obj->impl);
    bool nonblock = (flags & fs::O_NONBLOCK) != 0;
    return ring_buffer_read(rb, static_cast<uint8_t*>(kdst), count, nonblock);
}

__PRIVILEGED_CODE static ssize_t terminal_write(
    resource::resource_object*, const void* ksrc, size_t count, uint32_t
) {
    serial::write(static_cast<const char*>(ksrc), count);
    return static_cast<ssize_t>(count);
}

static void terminal_close(resource::resource_object*) {
}

__PRIVILEGED_CODE static uint32_t terminal_poll(
    resource::resource_object* obj, sync::poll_table* pt
) {
    if (!obj || !obj->impl) return sync::POLL_NVAL;

    auto* rb = static_cast<ring_buffer*>(obj->impl);
    return ring_buffer_poll_read(rb, pt) | sync::POLL_OUT;
}

static const resource::resource_ops g_terminal_ops = {
    terminal_read,
    terminal_write,
    terminal_close,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    terminal_poll,
    nullptr,
};

const resource::resource_ops* get_terminal_ops() {
    return &g_terminal_ops;
}

int32_t set_mode(uint32_t cmd) {
    return ld_set_mode(&g_console.ld, cmd);
}

int32_t console_ioctl(uint32_t cmd, uint64_t arg) {
    // Failures use fs error codes: this sits on the /dev/console node
    // path, where the terminal ERR value would read as fs::ERR_NOENT
    if (cmd == TIOCGPGRP) {
        int32_t g = 0;
        RUN_ELEVATED({
            g = static_cast<int32_t>(g_console.fg_group.load_acquire());
        });

        return mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &g, sizeof(g)) == mm::uaccess::OK
            ? OK : fs::ERR_INVAL;
    }

    if (cmd == TIOCSPGRP) {
        int32_t g = 0;
        if (mm::uaccess::copy_from_user(
                &g, reinterpret_cast<const void*>(arg), sizeof(g)) != mm::uaccess::OK
            || g < 0) {
            return fs::ERR_INVAL;
        }

        int32_t result = OK;
        RUN_ELEVATED({
            // POSIX requires an existing process group, 0 clears the foreground
            if (g > 0 && signals::send_to_group_id(
                    static_cast<uint32_t>(g), 0) != signals::OK) {
                result = fs::ERR_INVAL;
            } else {
                g_console.fg_group.store_release(static_cast<uint32_t>(g));
            }
        });

        return result;
    }

    return set_mode(cmd);
}

} // namespace terminal
