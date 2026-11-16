#ifndef STELLUX_TERMINAL_TTY_H
#define STELLUX_TERMINAL_TTY_H

#include "common/types.h"

/*
 * The Linux terminal requests that musl's termios calls and isatty() make through ioctl, and the settings they carry,
 * with Linux's numbers, layouts and flag values. Stellux's own mode requests are in terminal.h.
 */

namespace terminal {

constexpr uint32_t TCGETS     = 0x5401;
constexpr uint32_t TCSETS     = 0x5402;
constexpr uint32_t TCSETSW    = 0x5403;
constexpr uint32_t TCSETSF    = 0x5404;
constexpr uint32_t TIOCGPGRP  = 0x540F;
constexpr uint32_t TIOCSPGRP  = 0x5410;
constexpr uint32_t TIOCGWINSZ = 0x5413;
constexpr uint32_t TIOCSWINSZ = 0x5414;

// The control characters the settings carry, the first 19 of the 32 in musl's termios
constexpr size_t NCCS = 19;

// The settings TCGETS and TCSETS carry
struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;    // control flags: speed, character format and receiving
    uint32_t c_lflag;    // local flags: line editing, echo and signals
    uint8_t  c_line;
    uint8_t  c_cc[NCCS]; // control characters, VMIN and VTIME among them
};

// A terminal's size in characters, and in pixels where the terminal knows them
struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static_assert(sizeof(termios) == 36, "termios must match the layout programs pass");
static_assert(sizeof(winsize) == 8, "winsize must match the layout programs pass");

// The bits of c_lflag the line discipline implements
constexpr uint32_t ISIG   = 0x00000001; // turns Ctrl C and the other signal characters into signals
constexpr uint32_t ICANON = 0x00000002; // hands input over a line at a time, with editing
constexpr uint32_t ECHO   = 0x00000008; // echoes input

} // namespace terminal

#endif // STELLUX_TERMINAL_TTY_H
