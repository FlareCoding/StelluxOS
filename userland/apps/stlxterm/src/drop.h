#ifndef STLXTERM_DROP_H
#define STLXTERM_DROP_H

#include <stddef.h>
#include <stdint.h>
#include <stlxwin/stlxwin.h>

// The most bytes of another computer's name a paste keeps for its pill
#define DROP_HOST_MAX 64

// What a drop onto the terminal pastes
typedef enum {
    DROP_REFUSED, // nothing the terminal pastes
    DROP_FILES,   // a URI list, pasted as quoted paths
    DROP_TEXT,    // text, pasted as a paste is
} drop_kind_t;

// The paste a URI list makes, and what it left out
typedef struct {
    char *text;               // the quoted names, each followed by a space, NUL-terminated and malloc'd
    size_t len;
    uint32_t pasted;          // URIs pasted
    uint32_t remote;          // file URIs naming another computer, left out
    uint32_t unsafe;          // URIs that decode to nothing or to a control character, left out
    char host[DROP_HOST_MAX]; // the first other computer named
} drop_paste_t;

// The kind of a drag holding `types`, files winning over text, and in *out_mime the type to read for it
drop_kind_t drop_kind(const stlxwin_offer_types *types, const char **out_mime);

// Quotes `len` bytes of `name` for the Stellux shell and POSIX shells alike into out, which must hold 4 * len + 3
// bytes. Returns the bytes written before the NUL, 0 for a name that is empty or holds a control character.
size_t drop_quote(const char *name, size_t len, char *out);

// The paste of a URI list: each file URI on this computer as its percent-decoded path, any other URI as written,
// each quoted. Returns 0, or -1 when memory runs out.
int drop_paste_uri_list(const char *list, size_t len, drop_paste_t *out);

#endif // STLXTERM_DROP_H
