#include "paste.h"
#include <string.h>

#define PASTE_START "\x1b[200~"
#define PASTE_END   "\x1b[201~"

_Static_assert(sizeof(PASTE_START) - 1 == PASTE_MARKER_LEN && sizeof(PASTE_END) - 1 == PASTE_MARKER_LEN,
               "marker length");

size_t paste_encode(const char *text, size_t len, int bracketed, char *out) {
    size_t n = 0;
    if (bracketed) {
        memcpy(out, PASTE_START, PASTE_MARKER_LEN);
        n += PASTE_MARKER_LEN;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r' && i + 1 < len && text[i + 1] == '\n') {
            continue;
        }

        if (c == '\r' || c == '\n') {
            out[n++] = '\r';
        } else if (c == '\t' || (c >= 0x20 && c != 0x7F)) {
            out[n++] = (char)c;
        }
    }

    if (bracketed) {
        memcpy(out + n, PASTE_END, PASTE_MARKER_LEN);
        n += PASTE_MARKER_LEN;
    }

    return n;
}
