/* GETs over one connection per hop. Requests use HTTP/1.0, whose replies are never chunked, so a body ends at
 * its stated length or, without one, at the connection's clean close. */
#define _POSIX_C_SOURCE 200809L

#include <stlxhttp/http.h>
#include <stlxhttp/internal/reply.h>
#include <stlxhttp/internal/url.h>

#include <bearssl/bearssl.h>
#include <bearssl/trust_anchors.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#define HTTP_STATUS_OK   200
#define RECEIVE_CHUNK    4096
#define REQUEST_TEXT_MAX (STLXHTTP_URL_MAX + STLXHTTP_HOST_MAX + 64)
#define MESSAGE_MAX      512
#define TLS_SEED_SIZE    32
#define MS_PER_SECOND    1000

#define TLS_ERROR(name) { BR_ERR_##name, #name }

typedef struct {
    int         code;
    const char* name;
} tls_error_name;

struct stlxhttp_request {
    stlxhttp_url            url;
    int                     fd;
    int32_t                 socket_error;  /* why a socket call failed, read once BearSSL reports BR_ERR_IO */
    br_ssl_client_context   tls;
    br_x509_minimal_context x509;
    br_sslio_context        tls_io;
    unsigned char           tls_buffer[BR_SSL_BUFSIZE_BIDI];
    stlxhttp_reply          reply;
    uint8_t                 received[RECEIVE_CHUNK];  /* read with the header, handed out before reading more */
    size_t                  received_start;
    size_t                  received_end;
    char                    text[REQUEST_TEXT_MAX];
    char                    message[MESSAGE_MAX];
};

static const int32_t REDIRECT_STATUSES[] = { 301, 302, 303, 307, 308 };

static const tls_error_name TLS_ERROR_NAMES[] = {
    TLS_ERROR(BAD_PARAM), TLS_ERROR(BAD_STATE), TLS_ERROR(UNSUPPORTED_VERSION), TLS_ERROR(BAD_VERSION),
    TLS_ERROR(BAD_LENGTH), TLS_ERROR(TOO_LARGE), TLS_ERROR(BAD_MAC), TLS_ERROR(NO_RANDOM),
    TLS_ERROR(UNKNOWN_TYPE), TLS_ERROR(UNEXPECTED), TLS_ERROR(BAD_CCS), TLS_ERROR(BAD_ALERT),
    TLS_ERROR(BAD_HANDSHAKE), TLS_ERROR(OVERSIZED_ID), TLS_ERROR(BAD_CIPHER_SUITE), TLS_ERROR(BAD_COMPRESSION),
    TLS_ERROR(BAD_FRAGLEN), TLS_ERROR(BAD_SECRENEG), TLS_ERROR(EXTRA_EXTENSION), TLS_ERROR(BAD_SNI),
    TLS_ERROR(BAD_HELLO_DONE), TLS_ERROR(LIMIT_EXCEEDED), TLS_ERROR(BAD_FINISHED), TLS_ERROR(RESUME_MISMATCH),
    TLS_ERROR(INVALID_ALGORITHM), TLS_ERROR(BAD_SIGNATURE), TLS_ERROR(WRONG_KEY_USAGE), TLS_ERROR(NO_CLIENT_AUTH),
    TLS_ERROR(IO), TLS_ERROR(X509_INVALID_VALUE), TLS_ERROR(X509_TRUNCATED), TLS_ERROR(X509_EMPTY_CHAIN),
    TLS_ERROR(X509_INNER_TRUNC), TLS_ERROR(X509_BAD_TAG_CLASS), TLS_ERROR(X509_BAD_TAG_VALUE),
    TLS_ERROR(X509_INDEFINITE_LENGTH), TLS_ERROR(X509_EXTRA_ELEMENT), TLS_ERROR(X509_UNEXPECTED),
    TLS_ERROR(X509_NOT_CONSTRUCTED), TLS_ERROR(X509_NOT_PRIMITIVE), TLS_ERROR(X509_PARTIAL_BYTE),
    TLS_ERROR(X509_BAD_BOOLEAN), TLS_ERROR(X509_OVERFLOW), TLS_ERROR(X509_BAD_DN), TLS_ERROR(X509_BAD_TIME),
    TLS_ERROR(X509_UNSUPPORTED), TLS_ERROR(X509_LIMIT_EXCEEDED), TLS_ERROR(X509_WRONG_KEY_TYPE),
    TLS_ERROR(X509_BAD_SIGNATURE), TLS_ERROR(X509_TIME_UNKNOWN), TLS_ERROR(X509_EXPIRED),
    TLS_ERROR(X509_DN_MISMATCH), TLS_ERROR(X509_BAD_SERVER_NAME), TLS_ERROR(X509_CRITICAL_EXTENSION),
    TLS_ERROR(X509_NOT_CA), TLS_ERROR(X509_FORBIDDEN_KEY_USAGE), TLS_ERROR(X509_WEAK_PUBLIC_KEY),
    TLS_ERROR(X509_NOT_TRUSTED),
};

static __attribute__((format(printf, 3, 4))) int32_t fail(stlxhttp_request* request, int32_t err,
                                                          const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(request->message, sizeof(request->message), format, args);
    va_end(args);

    return err;
}

static bool is_redirect(int32_t status) {
    for (size_t i = 0; i < sizeof(REDIRECT_STATUSES) / sizeof(REDIRECT_STATUSES[0]); i++) {
        if (REDIRECT_STATUSES[i] == status) {
            return true;
        }
    }

    return false;
}

static void close_connection(stlxhttp_request* request) {
    if (request->fd >= 0) {
        close(request->fd);
        request->fd = -1;
    }
}

static int32_t connect_to_host(stlxhttp_request* request) {
    const stlxhttp_url* url = &request->url;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo* addresses = NULL;
    if (getaddrinfo(url->host, NULL, &hints, &addresses) != 0) {
        return fail(request, STLXHTTP_ERR_RESOLVE, "cannot resolve %s", url->host);
    }

    struct sockaddr_in address = *(struct sockaddr_in*)addresses->ai_addr;
    address.sin_port = htons(url->port);
    freeaddrinfo(addresses);

    request->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (request->fd < 0) {
        return fail(request, STLXHTTP_ERR_CONNECT, "cannot open a socket (errno %d)", errno);
    }

    if (connect(request->fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        return fail(request, STLXHTTP_ERR_CONNECT, "cannot connect to %s port %u (errno %d)", url->host,
                    (unsigned)url->port, errno);
    }

    return STLXHTTP_OK;
}

/* Waits at most the idle timeout for the server, then reads what it sent, 0 at the end of the stream */
static int64_t receive(stlxhttp_request* request, uint8_t* buffer, size_t size) {
    struct pollfd watch = { .fd = request->fd, .events = POLLIN };
    int ready = poll(&watch, 1, STLXHTTP_IDLE_TIMEOUT_MS);
    if (ready == 0) {
        request->socket_error = fail(request, STLXHTTP_ERR_TIMEOUT, "%s sent nothing for %d s", request->url.host,
                                     STLXHTTP_IDLE_TIMEOUT_MS / MS_PER_SECOND);
        return -1;
    }

    ssize_t count = ready < 0 ? -1 : read(request->fd, buffer, size);
    if (count < 0) {
        request->socket_error = fail(request, STLXHTTP_ERR_IO, "reading from %s failed (errno %d)",
                                     request->url.host, errno);
        return -1;
    }

    return count;
}

/* A server that is gone fails the send instead of killing the caller with SIGPIPE */
static int64_t send_bytes(stlxhttp_request* request, const void* bytes, size_t size) {
    ssize_t count = send(request->fd, bytes, size, MSG_NOSIGNAL);
    if (count <= 0) {
        request->socket_error = fail(request, STLXHTTP_ERR_IO, "sending to %s failed (errno %d)",
                                     request->url.host, errno);
        return -1;
    }

    return count;
}

static int tls_read(void* context, unsigned char* buffer, size_t size) {
    stlxhttp_request* request = context;
    int64_t count = receive(request, buffer, size);
    if (count == 0) {
        request->socket_error = fail(request, STLXHTTP_ERR_TRUNCATED, "%s closed the connection without closing TLS",
                                     request->url.host);
    }

    return count > 0 ? (int)count : -1;
}

static int tls_write(void* context, const unsigned char* buffer, size_t size) {
    int64_t count = send_bytes(context, buffer, size);
    return count > 0 ? (int)count : -1;
}

/* The request's error for a failed BearSSL call, the socket's own when BearSSL blames its I/O */
static int32_t fail_tls(stlxhttp_request* request) {
    int err = br_ssl_engine_last_error(&request->tls.eng);
    if (err == BR_ERR_IO && request->socket_error != STLXHTTP_OK) {
        return request->socket_error;
    }

    for (size_t i = 0; i < sizeof(TLS_ERROR_NAMES) / sizeof(TLS_ERROR_NAMES[0]); i++) {
        if (TLS_ERROR_NAMES[i].code == err) {
            return fail(request, STLXHTTP_ERR_TLS, "TLS with %s failed with %s", request->url.host,
                        TLS_ERROR_NAMES[i].name);
        }
    }

    return fail(request, STLXHTTP_ERR_TLS, "TLS with %s failed with BearSSL error %d", request->url.host, err);
}

static int32_t start_tls(stlxhttp_request* request) {
    unsigned char seed[TLS_SEED_SIZE];
    if (getrandom(seed, sizeof(seed), 0) != (ssize_t)sizeof(seed)) {
        return fail(request, STLXHTTP_ERR_TLS, "no entropy for TLS (errno %d)", errno);
    }

    br_ssl_client_init_full(&request->tls, &request->x509, TAs, TAs_NUM);
    br_ssl_engine_set_buffer(&request->tls.eng, request->tls_buffer, sizeof(request->tls_buffer), 1);
    br_ssl_engine_inject_entropy(&request->tls.eng, seed, sizeof(seed));
    if (!br_ssl_client_reset(&request->tls, request->url.host, 0)) {
        return fail_tls(request);
    }

    br_sslio_init(&request->tls_io, &request->tls.eng, tls_read, request, tls_write, request);

    return STLXHTTP_OK;
}

static int32_t send_request(stlxhttp_request* request) {
    const stlxhttp_url* url = &request->url;
    uint16_t default_port = url->is_https ? STLXHTTP_HTTPS_PORT : STLXHTTP_HTTP_PORT;
    char port[sizeof(":65535")] = "";
    if (url->port != default_port) {
        snprintf(port, sizeof(port), ":%u", (unsigned)url->port);
    }

    int length = snprintf(request->text, sizeof(request->text),
                          "GET %s HTTP/1.0\r\nHost: %s%s\r\nConnection: close\r\n\r\n", url->path, url->host, port);
    if (url->is_https) {
        bool sent = br_sslio_write_all(&request->tls_io, request->text, (size_t)length) == 0 &&
                    br_sslio_flush(&request->tls_io) == 0;
        return sent ? STLXHTTP_OK : fail_tls(request);
    }

    int sent = 0;
    while (sent < length) {
        int64_t count = send_bytes(request, request->text + sent, (size_t)(length - sent));
        if (count < 0) {
            return request->socket_error;
        }

        sent += (int)count;
    }

    return STLXHTTP_OK;
}

/* Reads what the server sent, 0 once it ended the stream cleanly */
static int64_t transport_read(stlxhttp_request* request, uint8_t* buffer, size_t size) {
    if (!request->url.is_https) {
        int64_t count = receive(request, buffer, size);
        return count < 0 ? request->socket_error : count;
    }

    int count = br_sslio_read(&request->tls_io, buffer, size);
    if (count > 0) {
        return count;
    }

    if (br_ssl_engine_last_error(&request->tls.eng) == BR_ERR_OK) {
        return 0;
    }

    return fail_tls(request);
}

static int32_t read_header(stlxhttp_request* request) {
    stlxhttp_reply_init(&request->reply);
    while (request->reply.state != STLXHTTP_REPLY_BODY) {
        int64_t count = transport_read(request, request->received, sizeof(request->received));
        if (count < 0) {
            return (int32_t)count;
        }

        if (count == 0) {
            return fail(request, STLXHTTP_ERR_TRUNCATED, "%s closed the connection before its reply's header ended",
                        request->url.host);
        }

        int64_t taken = stlxhttp_reply_parse_header(&request->reply, request->received, (size_t)count);
        if (taken == STLXHTTP_ERR_URL_TOO_LONG) {
            return fail(request, STLXHTTP_ERR_URL_TOO_LONG, "%s redirected to a URL longer than %d characters",
                        request->url.host, STLXHTTP_URL_MAX);
        }

        if (taken < 0) {
            return fail(request, STLXHTTP_ERR_BAD_REPLY, "%s sent a malformed reply header", request->url.host);
        }

        request->received_start = (size_t)taken;
        request->received_end = (size_t)count;
    }

    return STLXHTTP_OK;
}

/* Connects to the request's URL, sends the GET and reads the reply's header */
static int32_t open_hop(stlxhttp_request* request) {
    close_connection(request);
    request->socket_error = STLXHTTP_OK;

    int32_t rc = connect_to_host(request);
    if (rc != STLXHTTP_OK) {
        return rc;
    }

    if (request->url.is_https) {
        rc = start_tls(request);
    }

    if (rc != STLXHTTP_OK) {
        return rc;
    }

    rc = send_request(request);
    if (rc != STLXHTTP_OK) {
        return rc;
    }

    return read_header(request);
}

static int32_t follow_redirect(stlxhttp_request* request) {
    stlxhttp_url target;
    int32_t rc = stlxhttp_url_resolve(&request->url, request->reply.location, &target);
    if (rc == STLXHTTP_ERR_DOWNGRADE) {
        return fail(request, rc, "%s redirected from https to http", request->url.host);
    }

    if (rc != STLXHTTP_OK) {
        return fail(request, rc, "%s redirected to %.256s, which is not an http or https URL", request->url.host,
                    request->reply.location);
    }

    request->url = target;

    return STLXHTTP_OK;
}

int32_t stlxhttp_open(const char* url, uint32_t flags, stlxhttp_request** out) {
    stlxhttp_request* request = calloc(1, sizeof(*request));
    *out = request;
    if (!request) {
        return STLXHTTP_ERR_NO_MEMORY;
    }

    request->fd = -1;
    int32_t rc = stlxhttp_url_parse(url, &request->url);
    if (rc != STLXHTTP_OK) {
        return fail(request, rc, "%.256s is not an http or https URL of at most %d characters", url,
                    STLXHTTP_URL_MAX);
    }

    for (int hop = 0; hop <= STLXHTTP_REDIRECTS_MAX; hop++) {
        rc = open_hop(request);
        if (rc != STLXHTTP_OK) {
            return rc;
        }

        int32_t status = request->reply.status;
        if (status == HTTP_STATUS_OK && request->reply.chunked) {
            return fail(request, STLXHTTP_ERR_CHUNKED, "%s sent a chunked reply", request->url.host);
        }

        if (status == HTTP_STATUS_OK) {
            return STLXHTTP_OK;
        }

        if (!is_redirect(status)) {
            return fail(request, STLXHTTP_ERR_STATUS, "%s answered %d", request->url.host, status);
        }

        if (request->reply.location[0] == '\0') {
            return fail(request, STLXHTTP_ERR_BAD_REPLY, "%s redirected without a Location", request->url.host);
        }

        if (flags & STLXHTTP_NO_FOLLOW) {
            return STLXHTTP_OK;
        }

        rc = follow_redirect(request);
        if (rc != STLXHTTP_OK) {
            return rc;
        }
    }

    return fail(request, STLXHTTP_ERR_TOO_MANY_REDIRECTS, "more than %d redirects", STLXHTTP_REDIRECTS_MAX);
}

int64_t stlxhttp_read(stlxhttp_request* request, void* buffer, size_t size) {
    if (stlxhttp_reply_body_complete(&request->reply)) {
        return 0;
    }

    size_t held = request->received_end - request->received_start;
    if (held > 0) {
        size_t count = stlxhttp_reply_take_body(&request->reply, held < size ? held : size);
        memcpy(buffer, request->received + request->received_start, count);
        request->received_start += count;
        return (int64_t)count;
    }

    int64_t count = transport_read(request, buffer, size);
    if (count == 0 && request->reply.content_length >= 0) {
        return fail(request, STLXHTTP_ERR_TRUNCATED, "%s ended the reply after %lld of %lld bytes",
                    request->url.host, (long long)request->reply.body_received,
                    (long long)request->reply.content_length);
    }

    if (count <= 0) {
        return count;
    }

    return (int64_t)stlxhttp_reply_take_body(&request->reply, (size_t)count);
}

const char* stlxhttp_location(const stlxhttp_request* request) {
    return is_redirect(request->reply.status) ? request->reply.location : NULL;
}

const char* stlxhttp_error_message(const stlxhttp_request* request) {
    return request ? request->message : "out of memory";
}

void stlxhttp_close(stlxhttp_request* request) {
    if (!request) {
        return;
    }

    close_connection(request);
    free(request);
}
