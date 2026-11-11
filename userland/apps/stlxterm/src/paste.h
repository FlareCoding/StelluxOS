#ifndef STLXTERM_PASTE_H
#define STLXTERM_PASTE_H

#include <stddef.h>

// The bytes of each bracketed paste marker, ESC [ 200 ~ before a paste and ESC [ 201 ~ after it
#define PASTE_MARKER_LEN 6

// Encodes pasted text into out, which must hold len + 2 * PASTE_MARKER_LEN bytes: every line break becomes the
// carriage return Enter sends, other control characters are dropped so a paste cannot act as keys, and with
// bracketed paste on the markers go around it. Returns the bytes written.
size_t paste_encode(const char *text, size_t len, int bracketed, char *out);

#endif // STLXTERM_PASTE_H
