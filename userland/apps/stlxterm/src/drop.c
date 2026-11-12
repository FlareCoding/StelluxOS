#include "drop.h"
#include <stdlib.h>
#include <string.h>

#define URI_LIST     "text/uri-list"
#define TEXT_UTF8    "text/plain;charset=utf-8"
#define TEXT_PLAIN   "text/plain"
#define FILE_SCHEME  "file://"
#define LOCALHOST    "localhost"
#define HEX_DIGITS   16

// What a drop reads, in the order the terminal prefers: the exact paths of a URI list over the same paths as text
static const char *const READ_ORDER[] = { URI_LIST, TEXT_UTF8, TEXT_PLAIN };
static const drop_kind_t READ_KINDS[] = { DROP_FILES, DROP_TEXT, DROP_TEXT };

static int is_control(unsigned char c) {
    return c < 0x20 || c == 0x7F;
}

// A byte both shells take as part of a bare word
static int is_bare(unsigned char c) {
    int is_alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    return is_alnum || (c != '\0' && strchr("_@%+=:,./-", c) != NULL);
}

// A byte POSIX shells still read inside double quotes, or the quote that would end them
static int is_double_quote_special(unsigned char c) {
    return c == '"' || c == '$' || c == '\\' || c == '`' || c == '!';
}

drop_kind_t drop_kind(const stlxwin_offer_types *types, const char **out_mime) {
    for (size_t i = 0; i < sizeof(READ_ORDER) / sizeof(READ_ORDER[0]); i++) {
        for (uint32_t t = 0; t < types->count; t++) {
            if (strcmp(types->types[t], READ_ORDER[i]) == 0) {
                *out_mime = READ_ORDER[i];
                return READ_KINDS[i];
            }
        }
    }

    *out_mime = NULL;
    return DROP_REFUSED;
}

// Quotes in single quotes, each ' closing them for an escaped quote and opening them again, as POSIX shells read it
static size_t quote_posix(const char *name, size_t len, char *out) {
    size_t n = 0;
    out[n++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            out[n++] = name[i];
        }
    }

    out[n++] = '\'';
    out[n] = '\0';
    return n;
}

// The Stellux shell takes a quote only where a word starts and never joins quoted parts, so a name goes bare when
// it can, whole in single quotes or else in double quotes when that reads the same in both, and in POSIX's form last
size_t drop_quote(const char *name, size_t len, char *out) {
    int is_all_bare = 1;
    int has_single = 0;
    int has_double_special = 0;
    if (len == 0) {
        return 0;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (is_control(c)) {
            return 0;
        }

        is_all_bare &= is_bare(c);
        has_single |= c == '\'';
        has_double_special |= is_double_quote_special(c);
    }

    if (is_all_bare) {
        memcpy(out, name, len);
        out[len] = '\0';
        return len;
    }

    if (has_single && has_double_special) {
        return quote_posix(name, len, out);
    }

    char quote = has_single ? '"' : '\'';
    out[0] = quote;
    memcpy(out + 1, name, len);
    out[len + 1] = quote;
    out[len + 2] = '\0';
    return len + 2;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// Decodes the %XX escapes of `len` bytes into out, keeping a % that starts none. Returns the bytes written.
static size_t percent_decode(const char *in, size_t len, char *out) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        int high = in[i] == '%' && i + 2 < len ? hex_value(in[i + 1]) : -1;
        int low = high >= 0 ? hex_value(in[i + 2]) : -1;
        if (low >= 0) {
            out[n++] = (char)(high * HEX_DIGITS + low);
            i += 2;
        } else {
            out[n++] = in[i];
        }
    }

    return n;
}

// Adds a quoted name and the space after it to the paste. Returns 0, or -1 when memory runs out.
static int append(drop_paste_t *out, size_t *cap, const char *quoted, size_t len) {
    if (out->len + len + 2 > *cap) {
        size_t grown_cap = (*cap ? *cap : 64) * 2 + len;
        char *grown = realloc(out->text, grown_cap);
        if (!grown) {
            return -1;
        }

        out->text = grown;
        *cap = grown_cap;
    }

    memcpy(out->text + out->len, quoted, len);
    out->len += len;
    out->text[out->len++] = ' ';
    out->text[out->len] = '\0';
    return 0;
}

// A file URI names its computer between the scheme and the path, where nothing or localhost means this one
static void note_remote(drop_paste_t *out, const char *host, size_t host_len) {
    if (out->remote++ > 0) {
        return;
    }

    size_t kept = host_len < DROP_HOST_MAX - 1 ? host_len : DROP_HOST_MAX - 1;
    memcpy(out->host, host, kept);
    out->host[kept] = '\0';
}

static int paste_uri(drop_paste_t *out, size_t *cap, const char *uri, size_t len) {
    const char *name = uri;
    size_t name_len = len;
    char *decoded = NULL;
    size_t scheme_len = sizeof(FILE_SCHEME) - 1;
    if (len >= scheme_len && memcmp(uri, FILE_SCHEME, scheme_len) == 0) {
        const char *host = uri + scheme_len;
        const char *path = memchr(host, '/', len - scheme_len);
        size_t host_len = path ? (size_t)(path - host) : len - scheme_len;
        int is_here = host_len == 0 || (host_len == sizeof(LOCALHOST) - 1 && memcmp(host, LOCALHOST, host_len) == 0);
        if (!is_here) {
            note_remote(out, host, host_len);
            return 0;
        }

        decoded = malloc(len + 1);
        if (!decoded) {
            return -1;
        }

        name = decoded;
        name_len = path ? percent_decode(path, (size_t)(uri + len - path), decoded) : 0;
    }

    char *quoted = malloc(4 * name_len + 3);
    if (!quoted) {
        free(decoded);
        return -1;
    }

    size_t quoted_len = drop_quote(name, name_len, quoted);
    int rc = 0;
    if (quoted_len == 0) {
        out->unsafe++;
    } else {
        rc = append(out, cap, quoted, quoted_len);
        out->pasted += rc == 0 ? 1 : 0;
    }

    free(quoted);
    free(decoded);
    return rc;
}

// A URI list holds one URI per line, CR LF or LF ended, with # starting a comment line
int drop_paste_uri_list(const char *list, size_t len, drop_paste_t *out) {
    memset(out, 0, sizeof(*out));
    size_t cap = 0;
    const char *end = list + len;
    const char *line = list;
    while (line < end) {
        const char *stop = memchr(line, '\n', (size_t)(end - line));
        stop = stop ? stop : end;
        const char *last = stop > line && stop[-1] == '\r' ? stop - 1 : stop;
        size_t line_len = (size_t)(last - line);
        if (line_len > 0 && line[0] != '#' && paste_uri(out, &cap, line, line_len) != 0) {
            free(out->text);
            out->text = NULL;
            return -1;
        }

        line = stop < end ? stop + 1 : end;
    }

    return 0;
}
