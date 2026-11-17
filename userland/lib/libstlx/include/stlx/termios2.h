#ifndef STLX_TERMIOS2_H
#define STLX_TERMIOS2_H

/* A serial port's settings as the termios2 requests carry them, speeds in baud, with kernel/terminal/tty.h's values.
 * musl has no termios2, and host tests build this file on other systems, so it includes only the compiler's headers */

#include <stddef.h>
#include <stdint.h>

#define STLX_TERMIOS2_NCCS 19

struct stlx_termios2 {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[STLX_TERMIOS2_NCCS];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

_Static_assert(offsetof(struct stlx_termios2, c_ispeed) == 36, "stlx_termios2 must start with the termios layout");
_Static_assert(sizeof(struct stlx_termios2) == 44, "stlx_termios2 ABI size mismatch");

#define STLX_TCGETS2  0x802C542Au
#define STLX_TCSETS2  0x402C542Bu
#define STLX_TCSETSW2 0x402C542Cu
#define STLX_TCSETSF2 0x402C542Du

#define STLX_CBAUD   0x0000100Fu // c_cflag: the output speed's code
#define STLX_BOTHER  0x00001000u // the code whose speed is c_ospeed
#define STLX_CSIZE   0x00000030u
#define STLX_CS5     0x00000000u
#define STLX_CS6     0x00000010u
#define STLX_CS7     0x00000020u
#define STLX_CS8     0x00000030u
#define STLX_CSTOPB  0x00000040u
#define STLX_CREAD   0x00000080u
#define STLX_PARENB  0x00000100u
#define STLX_PARODD  0x00000200u
#define STLX_HUPCL   0x00000400u
#define STLX_CIBAUD  0x100F0000u // the input speed's code, 0 to follow the output speed
#define STLX_CMSPAR  0x40000000u
#define STLX_CRTSCTS 0x80000000u

#define STLX_VTIME 5
#define STLX_VMIN  6

#endif /* STLX_TERMIOS2_H */
