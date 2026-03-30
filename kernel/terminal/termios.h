#ifndef STELLUX_TERMINAL_TERMIOS_H
#define STELLUX_TERMINAL_TERMIOS_H

#include "common/types.h"

namespace terminal {

/**
 * @brief Linux-compatible termios structure.
 *
 * Binary-compatible with musl libc's struct termios so that
 * tcgetattr()/tcsetattr() work transparently.
 *
 * Layout (x86_64 and aarch64):
 *   uint32_t c_iflag;     // input mode flags
 *   uint32_t c_oflag;     // output mode flags
 *   uint32_t c_cflag;     // control mode flags
 *   uint32_t c_lflag;     // local mode flags
 *   uint8_t  c_line;      // line discipline
 *   uint8_t  c_cc[32];    // control characters
 *   uint32_t __c_ispeed;  // input speed
 *   uint32_t __c_ospeed;  // output speed
 */

constexpr size_t NCCS = 32;

struct kernel_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

static_assert(sizeof(kernel_termios) == 60, "kernel_termios size mismatch");

// ---- ioctl command numbers (Linux-compatible) ----
constexpr uint32_t TCGETS      = 0x5401;
constexpr uint32_t TCSETS      = 0x5402;   // TCSANOW
constexpr uint32_t TCSETSW     = 0x5403;   // TCSADRAIN
constexpr uint32_t TCSETSF     = 0x5404;   // TCSAFLUSH

constexpr uint32_t TIOCGWINSZ  = 0x5413;
constexpr uint32_t TIOCSWINSZ  = 0x5414;
constexpr uint32_t TIOCGPGRP   = 0x540F;   // used by isatty()
constexpr uint32_t TIOCSPGRP   = 0x5410;

// ---- c_iflag bits ----
constexpr uint32_t IGNBRK  = 0000001;
constexpr uint32_t BRKINT  = 0000002;
constexpr uint32_t IGNPAR  = 0000004;
constexpr uint32_t PARMRK  = 0000010;
constexpr uint32_t INPCK   = 0000020;
constexpr uint32_t ISTRIP  = 0000040;
constexpr uint32_t INLCR   = 0000100;
constexpr uint32_t IGNCR   = 0000200;
constexpr uint32_t ICRNL   = 0000400;
constexpr uint32_t IUCLC   = 0001000;
constexpr uint32_t IXON    = 0002000;
constexpr uint32_t IXANY   = 0004000;
constexpr uint32_t IXOFF   = 0010000;
constexpr uint32_t IMAXBEL = 0020000;
constexpr uint32_t IUTF8   = 0040000;

// ---- c_oflag bits ----
constexpr uint32_t OPOST   = 0000001;
constexpr uint32_t OLCUC   = 0000002;
constexpr uint32_t ONLCR   = 0000004;
constexpr uint32_t OCRNL   = 0000010;
constexpr uint32_t ONOCR   = 0000020;
constexpr uint32_t ONLRET  = 0000040;

// ---- c_cflag bits ----
constexpr uint32_t CSIZE   = 0000060;
constexpr uint32_t CS5     = 0000000;
constexpr uint32_t CS6     = 0000020;
constexpr uint32_t CS7     = 0000040;
constexpr uint32_t CS8     = 0000060;
constexpr uint32_t CSTOPB  = 0000100;
constexpr uint32_t CREAD   = 0000200;
constexpr uint32_t PARENB  = 0000400;
constexpr uint32_t PARODD  = 0001000;
constexpr uint32_t HUPCL   = 0002000;
constexpr uint32_t CLOCAL  = 0004000;

// ---- c_lflag bits ----
constexpr uint32_t ISIG    = 0000001;
constexpr uint32_t ICANON  = 0000002;
constexpr uint32_t ECHO_F  = 0000010;   // named ECHO_F to avoid macro collisions
constexpr uint32_t ECHOE   = 0000020;
constexpr uint32_t ECHOK   = 0000040;
constexpr uint32_t ECHONL  = 0000100;
constexpr uint32_t NOFLSH  = 0000200;
constexpr uint32_t TOSTOP  = 0000400;
constexpr uint32_t IEXTEN  = 0100000;

// ---- c_cc indices ----
constexpr uint8_t VINTR    = 0;
constexpr uint8_t VQUIT    = 1;
constexpr uint8_t VERASE   = 2;
constexpr uint8_t VKILL    = 3;
constexpr uint8_t VEOF     = 4;
constexpr uint8_t VTIME    = 5;
constexpr uint8_t VMIN     = 6;
constexpr uint8_t VSWTC    = 7;
constexpr uint8_t VSTART   = 8;
constexpr uint8_t VSTOP    = 9;
constexpr uint8_t VSUSP    = 10;
constexpr uint8_t VEOL     = 11;
constexpr uint8_t VREPRINT = 12;
constexpr uint8_t VDISCARD = 13;
constexpr uint8_t VWERASE  = 14;
constexpr uint8_t VLNEXT   = 15;
constexpr uint8_t VEOL2    = 16;

// ---- Baud rates (B38400 default for virtual terminals) ----
constexpr uint32_t B38400   = 0000017;

// ---- struct winsize (Linux-compatible) ----
struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static_assert(sizeof(winsize) == 8, "winsize size mismatch");

/**
 * @brief Initialize a kernel_termios to the "cooked" default.
 *
 * Default: ICANON | ECHO | ISIG | ICRNL | OPOST | ONLCR,
 * standard control character defaults.
 */
inline void termios_init_default(kernel_termios* t) {
    t->c_iflag = ICRNL | IUTF8;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD | CLOCAL | B38400;
    t->c_lflag = ISIG | ICANON | ECHO_F | ECHOE | ECHOK | IEXTEN;
    t->c_line  = 0;

    for (size_t i = 0; i < NCCS; i++)
        t->c_cc[i] = 0;

    t->c_cc[VINTR]    = 0x03;   // Ctrl-C
    t->c_cc[VQUIT]    = 0x1C;   // Ctrl-backslash
    t->c_cc[VERASE]   = 0x7F;   // DEL
    t->c_cc[VKILL]    = 0x15;   // Ctrl-U
    t->c_cc[VEOF]     = 0x04;   // Ctrl-D
    t->c_cc[VTIME]    = 0;
    t->c_cc[VMIN]     = 1;
    t->c_cc[VSTART]   = 0x11;   // Ctrl-Q
    t->c_cc[VSTOP]    = 0x13;   // Ctrl-S
    t->c_cc[VSUSP]    = 0x1A;   // Ctrl-Z
    t->c_cc[VEOL]     = 0;
    t->c_cc[VREPRINT] = 0x12;   // Ctrl-R
    t->c_cc[VDISCARD] = 0x0F;   // Ctrl-O
    t->c_cc[VWERASE]  = 0x17;   // Ctrl-W
    t->c_cc[VLNEXT]   = 0x16;   // Ctrl-V
    t->c_cc[VEOL2]    = 0;

    t->c_ispeed = B38400;
    t->c_ospeed = B38400;
}

/**
 * @brief Check if a termios configuration represents "raw mode"
 * (no ICANON, no ECHO).
 */
inline bool termios_is_raw(const kernel_termios* t) {
    return (t->c_lflag & (ICANON | ECHO_F)) == 0;
}

} // namespace terminal

#endif // STELLUX_TERMINAL_TERMIOS_H
