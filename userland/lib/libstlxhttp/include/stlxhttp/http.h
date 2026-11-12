#ifndef STLXHTTP_HTTP_H
#define STLXHTTP_HTTP_H

/* GETs over HTTP and HTTPS that follow redirects and check a body's end against its stated length. A request owns
 * its connection, TLS state and buffers from open to close, belongs to one thread and never writes files. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    STLXHTTP_OK                     = 0,
    STLXHTTP_ERR_NO_MEMORY          = -1,
    STLXHTTP_ERR_BAD_URL            = -2,  /* not an http or https URL of the form scheme://host[:port]/path */
    STLXHTTP_ERR_URL_TOO_LONG       = -3,
    STLXHTTP_ERR_RESOLVE            = -4,
    STLXHTTP_ERR_CONNECT            = -5,
    STLXHTTP_ERR_TLS                = -6,
    STLXHTTP_ERR_IO                 = -7,
    STLXHTTP_ERR_TIMEOUT            = -8,
    STLXHTTP_ERR_BAD_REPLY          = -9,
    STLXHTTP_ERR_STATUS             = -10, /* the final reply's status is not 200 */
    STLXHTTP_ERR_TOO_MANY_REDIRECTS = -11,
    STLXHTTP_ERR_DOWNGRADE          = -12, /* a redirect from https to http */
    STLXHTTP_ERR_CHUNKED            = -13,
    STLXHTTP_ERR_TRUNCATED          = -14, /* the reply ended before its stated length or without a TLS close */
};

#define STLXHTTP_URL_MAX         8192  /* the longest URL a request opens or follows, in characters */
#define STLXHTTP_REDIRECTS_MAX   5
#define STLXHTTP_IDLE_TIMEOUT_MS 30000 /* the longest a read waits for the server to send anything */

enum {
    STLXHTTP_NO_FOLLOW = 1u << 0,  /* a redirect reply opens like a 200, with its target in stlxhttp_location */
};

typedef struct stlxhttp_request stlxhttp_request;

/**
 * @brief GETs `url`, following its redirects, and reads the final reply's header.
 *
 * Succeeds once the final reply's status is 200 and its body can be read. With
 * `STLXHTTP_NO_FOLLOW` in `flags`, a first reply whose status is a redirect is
 * final as well. `*out` receives the request even when the open fails, so its
 * failure can be described, unless memory ran out, and must be passed to
 * `stlxhttp_close` either way.
 * @return STLXHTTP_OK or a negative STLXHTTP_ERR_* code.
 */
int32_t stlxhttp_open(const char* url, uint32_t flags, stlxhttp_request** out);

/**
 * @brief Reads up to `size` bytes of the body into `buffer`.
 * @return the number of bytes read, 0 at the end of a body whose length checks
 *         out, or a negative STLXHTTP_ERR_* code.
 */
int64_t stlxhttp_read(stlxhttp_request* request, void* buffer, size_t size);

/**
 * @brief The target of a redirect opened with `STLXHTTP_NO_FOLLOW`, as the
 * reply's Location header states it, an absolute URL or an absolute path.
 * @return the target, or NULL when the reply is not a redirect.
 */
const char* stlxhttp_location(const stlxhttp_request* request);

/**
 * @brief The length the final reply states for its body.
 * @return the length in bytes, or -1 when the reply states none.
 */
int64_t stlxhttp_content_length(const stlxhttp_request* request);

/**
 * @brief Describes the last failure of a call on `request`, naming the host,
 * status or byte counts it involved.
 * @return a message owned by the request, or the one for running out of memory
 *         when `request` is NULL.
 */
const char* stlxhttp_error_message(const stlxhttp_request* request);

/**
 * @brief Closes the request's connection and frees it. NULL is ignored.
 */
void stlxhttp_close(stlxhttp_request* request);

#ifdef __cplusplus
}
#endif

#endif /* STLXHTTP_HTTP_H */
