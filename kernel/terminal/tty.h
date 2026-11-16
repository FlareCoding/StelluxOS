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

// termios followed by both speeds in baud, for speeds no speed code names
struct termios2 {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
    uint32_t c_ispeed;   // input speed in baud, used when CIBAUD holds BOTHER
    uint32_t c_ospeed;   // output speed in baud, used when CBAUD holds BOTHER
};

// A terminal's size in characters, and in pixels where the terminal knows them
struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static_assert(sizeof(termios) == 36, "termios must match the layout programs pass");
static_assert(__builtin_offsetof(termios2, c_ispeed) == sizeof(termios), "termios2 must start with the termios layout");
static_assert(sizeof(termios2) == 44, "termios2 must match the layout programs pass");
static_assert(sizeof(winsize) == 8, "winsize must match the layout programs pass");

// The bits of c_lflag the line discipline implements
constexpr uint32_t ISIG   = 0x00000001; // turns Ctrl C and the other signal characters into signals
constexpr uint32_t ICANON = 0x00000002; // hands input over a line at a time, with editing
constexpr uint32_t ECHO   = 0x00000008; // echoes input

// A bit of c_lflag the line discipline leaves out
constexpr uint32_t ECHONL = 0x00000040; // echoes newlines

// Bits of c_iflag
constexpr uint32_t PARMRK = 0x00000008; // marks parity errors with 0xFF 0x00
constexpr uint32_t ISTRIP = 0x00000020; // clears each byte's top bit
constexpr uint32_t INLCR  = 0x00000040; // turns newlines into carriage returns
constexpr uint32_t IGNCR  = 0x00000080; // drops carriage returns
constexpr uint32_t ICRNL  = 0x00000100; // turns carriage returns into newlines
constexpr uint32_t IUCLC  = 0x00000200; // lowercases received letters
constexpr uint32_t IXON   = 0x00000400; // pauses output on XOFF
constexpr uint32_t IXOFF  = 0x00001000; // sends XOFF when unread input fills its buffer

// Bits of c_oflag
constexpr uint32_t OPOST = 0x00000001; // turns on the processing of written bytes

// Fields and bits of c_cflag
constexpr uint32_t CBAUD   = 0x0000100F; // the output speed's code
constexpr uint32_t CBAUDEX = 0x00001000; // set in BOTHER and the codes above B38400
constexpr uint32_t BOTHER  = 0x00001000; // the code whose speed is c_ospeed, or c_ispeed in CIBAUD
constexpr uint32_t B9600   = 0x0000000D;
constexpr uint32_t CSIZE   = 0x00000030; // data bits, one of CS5 to CS8
constexpr uint32_t CS5     = 0x00000000;
constexpr uint32_t CS6     = 0x00000010;
constexpr uint32_t CS7     = 0x00000020;
constexpr uint32_t CS8     = 0x00000030;
constexpr uint32_t CSTOPB  = 0x00000040; // 2 stop bits rather than 1
constexpr uint32_t CREAD   = 0x00000080; // turns receiving on
constexpr uint32_t PARENB  = 0x00000100; // parity on
constexpr uint32_t PARODD  = 0x00000200; // odd parity rather than even, or mark rather than space
constexpr uint32_t HUPCL   = 0x00000400; // lowers DTR and RTS at the last close
constexpr uint32_t CLOCAL  = 0x00000800; // ignores the carrier detect input
constexpr uint32_t CIBAUD  = 0x100F0000; // the input speed's code, 0 to follow the output speed
constexpr uint32_t CMSPAR  = 0x40000000; // mark or space parity
constexpr uint32_t CRTSCTS = 0x80000000; // flow control by CTS
constexpr uint32_t IBSHIFT = 16;         // CIBAUD's shift from CBAUD

// Entries of c_cc
constexpr size_t VTIME = 5; // how long a read waits, in tenths of a second
constexpr size_t VMIN  = 6; // how many bytes a read waits for

} // namespace terminal

#endif // STELLUX_TERMINAL_TTY_H
