#include "terminal/terminal.h"
#include "terminal/console_node.h"
#include "common/ring_buffer.h"
#include "io/serial.h"
#include "resource/resource.h"
#include "common/logging.h"
#include "fs/fstypes.h"
#include "fs/devfs/devfs.h"
#include "mm/heap.h"
#include "sync/spinlock.h"

namespace terminal {

constexpr uint32_t MODE_RAW    = 1;
constexpr size_t LINE_BUF_MAX  = 1023; // reserve 1 byte for \n
constexpr size_t INPUT_RING_CAPACITY = 4096;

__PRIVILEGED_BSS static struct {
    ring_buffer* input_rb;
    uint32_t mode;
    char line_buf[LINE_BUF_MAX + 1];
    size_t line_len;
    char prev_char;
    sync::spinlock lock;
} g_console;

__PRIVILEGED_CODE int32_t init() {
    g_console.input_rb = ring_buffer_create(INPUT_RING_CAPACITY);
    if (!g_console.input_rb) {
        log::error("terminal: failed to create ring buffer");
        return ERR;
    }

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
    sync::irq_state irq = sync::spin_lock_irqsave(g_console.lock);

    if (g_console.prev_char == '\r' && c == '\n') {
        g_console.prev_char = c;
        sync::spin_unlock_irqrestore(g_console.lock, irq);
        return;
    }
    g_console.prev_char = c;

    if (g_console.mode == MODE_RAW) {
        sync::spin_unlock_irqrestore(g_console.lock, irq);
        uint8_t byte = static_cast<uint8_t>(c);
        (void)ring_buffer_write(g_console.input_rb, &byte, 1, true);
        return;
    }

    // Cooked mode
    if (c == '\r' || c == '\n') {
        g_console.line_buf[g_console.line_len] = '\n';
        size_t len = g_console.line_len + 1;
        sync::spin_unlock_irqrestore(g_console.lock, irq);
        (void)ring_buffer_write(g_console.input_rb,
                               reinterpret_cast<const uint8_t*>(g_console.line_buf),
                               len, true);
        g_console.line_len = 0;
        serial::write_char('\r');
        serial::write_char('\n');
    } else if (c == 0x7F || c == 0x08) {
        if (g_console.line_len > 0) {
            g_console.line_len--;
            sync::spin_unlock_irqrestore(g_console.lock, irq);
            serial::write_char('\b');
            serial::write_char(' ');
            serial::write_char('\b');
        } else {
            sync::spin_unlock_irqrestore(g_console.lock, irq);
        }
    } else if (c >= 0x20 && c <= 0x7E) {
        if (g_console.line_len < LINE_BUF_MAX) {
            g_console.line_buf[g_console.line_len++] = c;
            sync::spin_unlock_irqrestore(g_console.lock, irq);
            serial::write_char(c);
        } else {
            sync::spin_unlock_irqrestore(g_console.lock, irq);
        }
    } else {
        sync::spin_unlock_irqrestore(g_console.lock, irq);
    }
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

__PRIVILEGED_CODE static void terminal_close(resource::resource_object*) {
    // Console is a singleton -- never destroyed. No-op.
}

static const resource::resource_ops g_terminal_ops = {
    terminal_read,
    terminal_write,
    terminal_close,
    nullptr,
};

__PRIVILEGED_CODE const resource::resource_ops* get_terminal_ops() {
    return &g_terminal_ops;
}

__PRIVILEGED_CODE int32_t set_mode(uint32_t cmd) {
    uint32_t new_mode;
    if (cmd == STLX_TCSETS_RAW) {
        new_mode = MODE_RAW;
    } else if (cmd == STLX_TCSETS_COOKED) {
        new_mode = 0;
    } else {
        return ERR;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(g_console.lock);
    if (g_console.mode != new_mode) {
        g_console.line_len = 0;
        g_console.mode = new_mode;
    }
    sync::spin_unlock_irqrestore(g_console.lock, irq);
    return OK;
}

} // namespace terminal
