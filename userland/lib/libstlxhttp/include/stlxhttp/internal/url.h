#ifndef STLXHTTP_INTERNAL_URL_H
#define STLXHTTP_INTERNAL_URL_H

/* A URL split into the parts a connection and its request line need. Internal to the library, not installed to the
 * sysroot. */

#include <stlxhttp/http.h>

#include <stdbool.h>

#define STLXHTTP_HOST_MAX   253  /* the longest DNS name */
#define STLXHTTP_HTTP_PORT  80
#define STLXHTTP_HTTPS_PORT 443

typedef struct {
    bool     is_https;
    char     host[STLXHTTP_HOST_MAX + 1];
    uint16_t port;
    char     path[STLXHTTP_URL_MAX + 1]; /* from the first slash, query included and fragment dropped */
} stlxhttp_url;

/**
 * @brief Splits `text`, an http or https URL, into `url`. A missing path is `/`.
 * @return STLXHTTP_OK, STLXHTTP_ERR_URL_TOO_LONG or STLXHTTP_ERR_BAD_URL.
 */
int32_t stlxhttp_url_parse(const char* text, stlxhttp_url* url);

/**
 * @brief Finds where a redirect from `base` to `location`, an absolute URL or
 * an absolute path, leads.
 * @return STLXHTTP_OK, STLXHTTP_ERR_DOWNGRADE for a redirect from https to
 *         http, or an error of stlxhttp_url_parse.
 */
int32_t stlxhttp_url_resolve(const stlxhttp_url* base, const char* location, stlxhttp_url* target);

#endif /* STLXHTTP_INTERNAL_URL_H */
