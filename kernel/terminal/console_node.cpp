#include "terminal/console_node.h"
#include "terminal/terminal.h"
#include "terminal/termios.h"
#include "common/ring_buffer.h"
#include "io/serial.h"
#include "mm/uaccess.h"
#include "fs/fs.h"
#include "fs/file.h"

namespace terminal {

// Console-global termios and winsize state
static kernel_termios g_console_termios;
static winsize g_console_winsize;
static bool g_console_termios_initialized = false;

static void ensure_console_termios_init() {
    if (!g_console_termios_initialized) {
        termios_init_default(&g_console_termios);
        g_console_winsize.ws_row = 24;
        g_console_winsize.ws_col = 80;
        g_console_winsize.ws_xpixel = 0;
        g_console_winsize.ws_ypixel = 0;
        g_console_termios_initialized = true;
    }
}

console_node::console_node(fs::instance* fs, const char* name)
    : fs::node(fs::node_type::char_device, fs, name) {
    ensure_console_termios_init();
}

ssize_t console_node::read(fs::file* f, void* buf, size_t count) {
    bool nonblock = (f->flags() & fs::O_NONBLOCK) != 0;
    return ring_buffer_read(console_input_rb(),
                            static_cast<uint8_t*>(buf), count, nonblock);
}

ssize_t console_node::write(fs::file*, const void* buf, size_t count) {
    serial::write(static_cast<const char*>(buf), count);
    return static_cast<ssize_t>(count);
}

int32_t console_node::ioctl(fs::file*, uint32_t cmd, uint64_t arg) {
    ensure_console_termios_init();

    switch (cmd) {
    case TCGETS: {
        // Backward compat: ioctl(fd, 0x5401, 0) = legacy STLX_TCSETS_RAW
        if (arg == 0) {
            return terminal::set_mode(STLX_TCSETS_RAW);
        }
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &g_console_termios, sizeof(g_console_termios));
        return (rc == mm::uaccess::OK) ? fs::OK : fs::ERR_INVAL;
    }

    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        // Backward compat: ioctl(fd, 0x5402, 0) = legacy STLX_TCSETS_COOKED
        if (arg == 0) {
            return terminal::set_mode(STLX_TCSETS_COOKED);
        }
        kernel_termios new_termios;
        int32_t rc = mm::uaccess::copy_from_user(
            &new_termios, reinterpret_cast<const void*>(arg), sizeof(new_termios));
        if (rc != mm::uaccess::OK) return fs::ERR_INVAL;
        g_console_termios = new_termios;
        // Apply to line discipline
        bool canon = (new_termios.c_lflag & ICANON) != 0;
        terminal::set_mode(canon ? STLX_TCSETS_COOKED : STLX_TCSETS_RAW);
        return fs::OK;
    }

    case TIOCGWINSZ: {
        if (arg == 0) return fs::ERR_INVAL;
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &g_console_winsize, sizeof(g_console_winsize));
        return (rc == mm::uaccess::OK) ? fs::OK : fs::ERR_INVAL;
    }

    case TIOCSWINSZ: {
        if (arg == 0) return fs::ERR_INVAL;
        winsize ws;
        int32_t rc = mm::uaccess::copy_from_user(
            &ws, reinterpret_cast<const void*>(arg), sizeof(ws));
        if (rc != mm::uaccess::OK) return fs::ERR_INVAL;
        g_console_winsize = ws;
        return fs::OK;
    }

    case TIOCGPGRP: {
        // Stub: makes isatty() succeed
        if (arg == 0) return fs::OK;
        int32_t pgrp = 0;
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(arg), &pgrp, sizeof(pgrp));
        return (rc == mm::uaccess::OK) ? fs::OK : fs::ERR_INVAL;
    }

    default:
        return fs::ERR_NOSYS;
    }
}

int32_t console_node::getattr(fs::vattr* attr) {
    if (!attr) return fs::ERR_INVAL;
    attr->type = fs::node_type::char_device;
    attr->size = 0;
    return fs::OK;
}

} // namespace terminal
