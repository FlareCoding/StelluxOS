#ifndef STLXTERM_PASTE_H
#define STLXTERM_PASTE_H

#include <stddef.h>

// The bytes of each bracketed paste marker, ESC [ 200 ~ before a paste and ESC [ 201 ~ after it
#define PASTE_MARKER_LEN 6

// Pastes larger than this ask first, whatever the program
#define PASTE_ASK_BYTES 1000000

typedef enum {
    PASTE_SEND,      // straight to the program
    PASTE_ASK_LINES, // lines for a program without bracketed paste, which would run each as it arrives
    PASTE_ASK_SIZE,  // more than PASTE_ASK_BYTES
} paste_check_t;

// Encodes pasted text into out, which must hold len + 2 * PASTE_MARKER_LEN bytes: every line break becomes the
// carriage return Enter sends, other control characters are dropped so a paste cannot act as keys, and with
// bracketed paste on the markers go around it. Returns the bytes written.
size_t paste_encode(const char *text, size_t len, int bracketed, char *out);

// Whether a paste goes straight to the program or asks first
paste_check_t paste_check(const char *text, size_t len, int bracketed, int protects_lines);

// The lines a text holds, a final line break ending its last line rather than starting another
size_t paste_line_count(const char *text, size_t len);

#endif // STLXTERM_PASTE_H
