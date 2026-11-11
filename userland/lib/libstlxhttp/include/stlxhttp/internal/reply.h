#ifndef STLXHTTP_INTERNAL_REPLY_H
#define STLXHTTP_INTERNAL_REPLY_H

/* The reply to one request, parsed from its bytes as they arrive and without I/O, so its header may split across
 * reads anywhere. Internal to the library, not installed to the sysroot. */

#include <stlxhttp/http.h>

#include <stdbool.h>

#define STLXHTTP_REPLY_TEXT_MAX 32  /* kept of the status line, a header name or a length */

typedef enum {
    STLXHTTP_REPLY_STATUS_LINE,
    STLXHTTP_REPLY_HEADER_NAME,
    STLXHTTP_REPLY_LOCATION,
    STLXHTTP_REPLY_CONTENT_LENGTH,
    STLXHTTP_REPLY_SKIPPED_LINE,
    STLXHTTP_REPLY_BODY,
} stlxhttp_reply_state;

typedef struct {
    stlxhttp_reply_state state;
    char    text[STLXHTTP_REPLY_TEXT_MAX + 1];
    size_t  text_length;
    int32_t status;
    int64_t content_length;                 /* -1 when the reply states none */
    int64_t body_received;
    bool    chunked;
    char    location[STLXHTTP_URL_MAX + 1]; /* empty when the reply names none */
    size_t  location_length;
} stlxhttp_reply;

void stlxhttp_reply_init(stlxhttp_reply* reply);

/**
 * @brief Parses the header from `bytes`, stopping after the blank line that ends it.
 * @return the number of bytes taken, all of them while the header goes on, or
 *         STLXHTTP_ERR_BAD_REPLY or STLXHTTP_ERR_URL_TOO_LONG.
 */
int64_t stlxhttp_reply_parse_header(stlxhttp_reply* reply, const uint8_t* bytes, size_t length);

/**
 * @brief Counts `length` received body bytes.
 * @return how many of them belong to the body, fewer when they run past its
 *         stated length.
 */
size_t stlxhttp_reply_take_body(stlxhttp_reply* reply, size_t length);

/**
 * @brief Whether the body holds every byte its stated length promises, never
 * true for a body without one.
 */
bool stlxhttp_reply_body_complete(const stlxhttp_reply* reply);

#endif /* STLXHTTP_INTERNAL_REPLY_H */
