#include "terminal/terminal.h"
#include "common/ring_buffer.h"
#include "io/serial.h"
#include "resource/resource.h"
#include "common/logging.h"
#include "fs/fstypes.h"

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

    log::info("terminal: console initialized (cooked mode)");
    return OK;
}

__PRIVILEGED_CODE void input_char(char c) {
    // CR/LF coalescing: skip \n immediately after \r
    if (g_console.prev_char == '\r' && c == '\n') {
        g_console.prev_char = c;
        return;
    }
    g_console.prev_char = c;

    if (g_console.mode == MODE_RAW) {
        uint8_t byte = static_cast<uint8_t>(c);
        (void)ring_buffer_write(g_console.input_rb, &byte, 1, true);
        return;
    }

    // Cooked mode
    if (c == '\r' || c == '\n') {
        g_console.line_buf[g_console.line_len] = '\n';
        (void)ring_buffer_write(g_console.input_rb,
                               reinterpret_cast<const uint8_t*>(g_console.line_buf),
                               g_console.line_len + 1, true);
        g_console.line_len = 0;
        serial::write_char('\r');
        serial::write_char('\n');
    } else if (c == 0x7F || c == 0x08) {
        if (g_console.line_len > 0) {
            g_console.line_len--;
            serial::write_char('\b');
            serial::write_char(' ');
            serial::write_char('\b');
        }
    } else if (c >= 0x20 && c <= 0x7E) {
        if (g_console.line_len < LINE_BUF_MAX) {
            g_console.line_buf[g_console.line_len++] = c;
            serial::write_char(c);
        }
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
};

__PRIVILEGED_CODE const resource::resource_ops* get_terminal_ops() {
    return &g_terminal_ops;
}

} // namespace terminal
