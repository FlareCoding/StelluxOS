#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stlx/net.h>

#include <bearssl/bearssl.h>
#include <bearssl/trust_anchors.h>

#define DNS_FALLBACK_SERVER  0x08080808
#define DNS_PORT             53
#define DNS_HEADER_LEN       12
#define DNS_TYPE_A           1
#define DNS_CLASS_IN         1
#define DNS_FLAG_RD          0x0100
#define DNS_FLAG_QR          0x8000
#define DNS_RCODE_MASK       0x000F
#define DNS_MAX_PACKET       512
#define DNS_RECV_TIMEOUT_MS  100
#define DNS_RECV_RETRIES     50
#define DNS_SEND_ATTEMPTS    2

#define FETCH_BUF_SIZE       4096
#define HTTP_REQ_SIZE        2048
#define DEFAULT_HTTPS_PORT   443
#define DEFAULT_HTTP_PORT    80
#define DEFAULT_PATH         "/"

static uint32_t parse_ipv4(const char *str) {
    int field = 0;
    uint32_t val = 0;
    uint32_t parts[4] = {0, 0, 0, 0};

    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] >= '0' && str[i] <= '9') {
            val = val * 10 + (uint32_t)(str[i] - '0');
        } else if (str[i] == '.') {
            if (field >= 3 || val > 255) return 0;
            parts[field++] = val;
            val = 0;
        } else {
            return 0;
        }
    }
    if (field != 3 || val > 255) return 0;
    parts[3] = val;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

static int is_ip_address(const char *str) {
    for (int i = 0; str[i]; i++) {
        if (str[i] != '.' && (str[i] < '0' || str[i] > '9'))
            return 0;
    }
    return 1;
}

static void format_ip(uint32_t ip, char *buf, size_t sz) {
    snprintf(buf, sz, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

/* ---- DNS resolution ---- */

static uint16_t dns_htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static uint32_t dns_htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v & 0xFF0000) >> 8) | ((v & 0xFF000000) >> 24);
}

static int dns_encode_name(const char *name, uint8_t *buf, size_t buf_size) {
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 253) return -1;

    size_t pos = 0;
    const char *seg = name;

    while (*seg) {
        const char *dot = seg;
        while (*dot && *dot != '.') dot++;
        size_t seg_len = (size_t)(dot - seg);
        if (seg_len == 0 || seg_len > 63) return -1;
        if (pos + 1 + seg_len + 1 > buf_size) return -1;

        buf[pos++] = (uint8_t)seg_len;
        memcpy(buf + pos, seg, seg_len);
        pos += seg_len;

        seg = (*dot == '.') ? dot + 1 : dot;
    }
    buf[pos++] = 0;
    return (int)pos;
}

static int dns_skip_name(const uint8_t *pkt, size_t pkt_len, size_t offset) {
    size_t pos = offset;
    while (pos < pkt_len) {
        uint8_t label_len = pkt[pos];
        if (label_len == 0) { pos++; break; }
        if ((label_len & 0xC0) == 0xC0) { pos += 2; break; }
        pos += 1 + label_len;
    }
    if (pos > pkt_len) return -1;
    return (int)pos;
}

static uint32_t get_dns_server(void) {
    struct stlx_net_status st;
    if (stlx_net_get_status(&st) != 0) return DNS_FALLBACK_SERVER;

    const struct stlx_ifinfo *def = stlx_net_default_if(&st);
    if (def && def->ipv4_dns != 0) return def->ipv4_dns;
    return DNS_FALLBACK_SERVER;
}

static uint32_t dns_resolve(const char *hostname) {
    uint32_t server_ip = get_dns_server();
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    uint8_t query[DNS_MAX_PACKET];
    memset(query, 0, sizeof(query));

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint16_t txn_id = (uint16_t)(ts.tv_nsec & 0xFFFF);

    query[0] = (uint8_t)(txn_id >> 8);
    query[1] = (uint8_t)(txn_id & 0xFF);
    query[2] = (uint8_t)(DNS_FLAG_RD >> 8);
    query[3] = (uint8_t)(DNS_FLAG_RD & 0xFF);
    query[4] = 0; query[5] = 1;

    int name_len = dns_encode_name(hostname, query + DNS_HEADER_LEN,
                                   sizeof(query) - DNS_HEADER_LEN - 4);
    if (name_len < 0) { close(fd); return 0; }

    size_t q_end = (size_t)(DNS_HEADER_LEN + name_len);
    query[q_end]     = 0; query[q_end + 1] = DNS_TYPE_A;
    query[q_end + 2] = 0; query[q_end + 3] = DNS_CLASS_IN;
    size_t query_len = q_end + 4;

    struct sockaddr_in dns_addr;
    memset(&dns_addr, 0, sizeof(dns_addr));
    dns_addr.sin_family = AF_INET;
    dns_addr.sin_port = dns_htons(DNS_PORT);
    dns_addr.sin_addr.s_addr = dns_htonl(server_ip);

    uint32_t result = 0;

    for (int attempt = 0; attempt < DNS_SEND_ATTEMPTS && result == 0; attempt++) {
        ssize_t nsent = sendto(fd, query, query_len, 0,
                               (struct sockaddr *)&dns_addr, sizeof(dns_addr));
        if (nsent < 0) continue;

        for (int poll = 0; poll < DNS_RECV_RETRIES; poll++) {
            uint8_t resp[DNS_MAX_PACKET];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t nrecv = recvfrom(fd, resp, sizeof(resp), MSG_DONTWAIT,
                                     (struct sockaddr *)&from, &fromlen);
            if (nrecv < DNS_HEADER_LEN) {
                struct timespec delay = {0, DNS_RECV_TIMEOUT_MS * 1000000L};
                nanosleep(&delay, NULL);
                continue;
            }

            uint16_t resp_id = ((uint16_t)resp[0] << 8) | resp[1];
            if (resp_id != txn_id) continue;

            uint16_t flags = ((uint16_t)resp[2] << 8) | resp[3];
            if (!(flags & DNS_FLAG_QR)) continue;
            if ((flags & DNS_RCODE_MASK) != 0) break;

            uint16_t ancount = ((uint16_t)resp[6] << 8) | resp[7];
            if (ancount == 0) break;

            int pos = dns_skip_name(resp, (size_t)nrecv, DNS_HEADER_LEN);
            if (pos < 0) break;
            pos += 4;

            for (uint16_t a = 0; a < ancount && pos > 0; a++) {
                pos = dns_skip_name(resp, (size_t)nrecv, (size_t)pos);
                if (pos < 0 || pos + 10 > nrecv) break;

                uint16_t rr_type = ((uint16_t)resp[pos] << 8) | resp[pos + 1];
                uint16_t rdlength = ((uint16_t)resp[pos + 8] << 8) | resp[pos + 9];
                pos += 10;

                if (rr_type == DNS_TYPE_A && rdlength == 4 && pos + 4 <= nrecv) {
                    result = ((uint32_t)resp[pos] << 24) |
                             ((uint32_t)resp[pos + 1] << 16) |
                             ((uint32_t)resp[pos + 2] << 8) |
                             (uint32_t)resp[pos + 3];
                    break;
                }
                if (pos + (int)rdlength > (int)nrecv) break;
                pos += rdlength;
            }
            break;
        }
    }

    close(fd);
    return result;
}

/* ---- Shared helpers ---- */

static ssize_t write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0) return -1;
        p += n;
        remaining -= (size_t)n;
    }
    return (ssize_t)len;
}

static int tcp_connect(uint32_t ip_host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("fetch: socket() failed (errno=%d)\r\n", errno);
        return -1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = htonl(ip_host);

    char ip_str[16];
    format_ip(ip_host, ip_str, sizeof(ip_str));
    printf("fetch: connecting to %s:%u...\r\n", ip_str, port);

    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        printf("fetch: connect failed (errno=%d)\r\n", errno);
        close(fd);
        return -1;
    }

    return fd;
}

static int build_http_request(char *req, size_t req_size,
                              const char *host, const char *path) {
    return snprintf(req, req_size,
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);
}

/* ---- HTTP header/body separation ----
 *
 * State machine to find the \r\n\r\n boundary between HTTP headers and
 * body. Handles the boundary being split across read() calls.
 *
 * States: 0=normal, 1=seen \r, 2=seen \r\n, 3=seen \r\n\r
 * When state reaches 4: boundary found.
 */
typedef struct {
    int state;        /* 0-3: matching progress for \r\n\r\n */
    int headers_done; /* 1 once the boundary has been found */
} http_split_state;

/*
 * Process a buffer of HTTP response data. Routes header bytes to stdout
 * and body bytes to out_fd.
 *
 * Returns the number of body bytes written to out_fd, or -1 on write error.
 */
static ssize_t process_response_chunk(http_split_state *hs, int out_fd,
                                       const char *buf, size_t len) {
    size_t body_written = 0;

    if (hs->headers_done) {
        /* Entire buffer is body */
        if (write_all(out_fd, buf, len) < 0) return -1;
        return (ssize_t)len;
    }

    for (size_t i = 0; i < len; i++) {
        char c = buf[i];

        if (hs->headers_done) {
            /* Rest of buffer is body */
            size_t remaining = len - i;
            if (write_all(out_fd, buf + i, remaining) < 0) return -1;
            body_written += remaining;
            break;
        }

        /* Advance the \r\n\r\n state machine.
         *
         * The boundary \r\n\r\n consists of: the end of the last header
         * line (\r\n) followed by a blank separator line (\r\n).
         * We print the first \r\n (header content) but suppress the
         * second \r\n (separator) from stdout output.
         */
        switch (hs->state) {
        case 0:
            if (c == '\r') hs->state = 1;
            else            hs->state = 0;
            break;
        case 1: /* seen \r */
            if (c == '\n') hs->state = 2;
            else if (c == '\r') hs->state = 1;
            else                hs->state = 0;
            break;
        case 2: /* seen \r\n */
            if (c == '\r') {
                hs->state = 3;
                continue; /* suppress separator \r from stdout */
            } else {
                hs->state = 0;
            }
            break;
        case 3: /* seen \r\n\r */
            if (c == '\n') {
                hs->state = 4;
                hs->headers_done = 1;
                continue; /* suppress separator \n from stdout */
            } else if (c == '\r') {
                /* False alarm: the \r at state 2 was content.
                   Flush the suppressed \r, then stay in state 1. */
                write(STDOUT_FILENO, "\r", 1);
                hs->state = 1;
            } else {
                /* False alarm: flush suppressed \r, reset. */
                write(STDOUT_FILENO, "\r", 1);
                hs->state = 0;
            }
            break;
        }

        /* Header bytes go to stdout for user feedback */
        if (!hs->headers_done) {
            write(STDOUT_FILENO, &c, 1);
        }
    }

    return (ssize_t)body_written;
}

/* ---- Plain HTTP fetch ---- */

static int fetch_plain(int fd, const char *host, const char *path, int out_fd) {
    char req[HTTP_REQ_SIZE];
    int req_len = build_http_request(req, sizeof(req), host, path);
    if (req_len < 0 || req_len >= (int)sizeof(req)) {
        printf("fetch: request too large\r\n");
        close(fd);
        return 1;
    }

    if (write_all(fd, req, (size_t)req_len) < 0) {
        printf("fetch: failed to send request (errno=%d)\r\n", errno);
        close(fd);
        return 1;
    }

    char buf[FETCH_BUF_SIZE];
    ssize_t n;
    size_t total = 0;

    if (out_fd >= 0) {
        /* Save mode: separate headers from body */
        http_split_state hs = {0, 0};
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            ssize_t body = process_response_chunk(&hs, out_fd, buf, (size_t)n);
            if (body < 0) {
                printf("\r\nfetch: write error\r\n");
                close(fd);
                return 1;
            }
            total += (size_t)body;
        }
    } else {
        /* Stdout mode: dump everything (existing behavior) */
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(STDOUT_FILENO, buf, (size_t)n);
            total += (size_t)n;
        }
    }

    if (total == 0 && out_fd < 0)
        printf("fetch: empty response\r\n");

    if (out_fd >= 0)
        printf("\r\n--- %zu body bytes saved ---\r\n", total);
    else
        printf("\r\n--- %zu bytes received ---\r\n", total);

    close(fd);
    return 0;
}

/* ---- TLS error reporting ---- */

static const char *tls_error_name(int err) {
    switch (err) {
    case BR_ERR_OK:                    return "OK";
    case BR_ERR_BAD_PARAM:             return "BAD_PARAM";
    case BR_ERR_BAD_STATE:             return "BAD_STATE";
    case BR_ERR_UNSUPPORTED_VERSION:   return "UNSUPPORTED_VERSION";
    case BR_ERR_BAD_VERSION:           return "BAD_VERSION";
    case BR_ERR_BAD_LENGTH:            return "BAD_LENGTH";
    case BR_ERR_TOO_LARGE:             return "TOO_LARGE";
    case BR_ERR_BAD_MAC:               return "BAD_MAC";
    case BR_ERR_NO_RANDOM:             return "NO_RANDOM";
    case BR_ERR_UNKNOWN_TYPE:          return "UNKNOWN_TYPE";
    case BR_ERR_UNEXPECTED:            return "UNEXPECTED";
    case BR_ERR_BAD_CCS:               return "BAD_CCS";
    case BR_ERR_BAD_ALERT:             return "BAD_ALERT";
    case BR_ERR_BAD_HANDSHAKE:         return "BAD_HANDSHAKE";
    case BR_ERR_OVERSIZED_ID:          return "OVERSIZED_ID";
    case BR_ERR_BAD_CIPHER_SUITE:      return "BAD_CIPHER_SUITE";
    case BR_ERR_BAD_COMPRESSION:       return "BAD_COMPRESSION";
    case BR_ERR_BAD_FRAGLEN:           return "BAD_FRAGLEN";
    case BR_ERR_BAD_SECRENEG:          return "BAD_SECRENEG";
    case BR_ERR_EXTRA_EXTENSION:       return "EXTRA_EXTENSION";
    case BR_ERR_BAD_SNI:               return "BAD_SNI";
    case BR_ERR_BAD_HELLO_DONE:        return "BAD_HELLO_DONE";
    case BR_ERR_LIMIT_EXCEEDED:        return "LIMIT_EXCEEDED";
    case BR_ERR_BAD_FINISHED:          return "BAD_FINISHED";
    case BR_ERR_RESUME_MISMATCH:       return "RESUME_MISMATCH";
    case BR_ERR_INVALID_ALGORITHM:     return "INVALID_ALGORITHM";
    case BR_ERR_BAD_SIGNATURE:         return "BAD_SIGNATURE";
    case BR_ERR_WRONG_KEY_USAGE:       return "WRONG_KEY_USAGE";
    case BR_ERR_NO_CLIENT_AUTH:        return "NO_CLIENT_AUTH";
    case BR_ERR_IO:                    return "IO";
    case BR_ERR_X509_INVALID_VALUE:    return "X509_INVALID_VALUE";
    case BR_ERR_X509_TRUNCATED:        return "X509_TRUNCATED";
    case BR_ERR_X509_EMPTY_CHAIN:      return "X509_EMPTY_CHAIN";
    case BR_ERR_X509_INNER_TRUNC:      return "X509_INNER_TRUNC";
    case BR_ERR_X509_BAD_TAG_CLASS:    return "X509_BAD_TAG_CLASS";
    case BR_ERR_X509_BAD_TAG_VALUE:    return "X509_BAD_TAG_VALUE";
    case BR_ERR_X509_INDEFINITE_LENGTH: return "X509_INDEFINITE_LENGTH";
    case BR_ERR_X509_EXTRA_ELEMENT:    return "X509_EXTRA_ELEMENT";
    case BR_ERR_X509_UNEXPECTED:       return "X509_UNEXPECTED";
    case BR_ERR_X509_NOT_CONSTRUCTED:  return "X509_NOT_CONSTRUCTED";
    case BR_ERR_X509_NOT_PRIMITIVE:    return "X509_NOT_PRIMITIVE";
    case BR_ERR_X509_PARTIAL_BYTE:     return "X509_PARTIAL_BYTE";
    case BR_ERR_X509_BAD_BOOLEAN:      return "X509_BAD_BOOLEAN";
    case BR_ERR_X509_OVERFLOW:         return "X509_OVERFLOW";
    case BR_ERR_X509_BAD_DN:           return "X509_BAD_DN";
    case BR_ERR_X509_BAD_TIME:         return "X509_BAD_TIME";
    case BR_ERR_X509_UNSUPPORTED:      return "X509_UNSUPPORTED";
    case BR_ERR_X509_LIMIT_EXCEEDED:   return "X509_LIMIT_EXCEEDED";
    case BR_ERR_X509_WRONG_KEY_TYPE:   return "X509_WRONG_KEY_TYPE";
    case BR_ERR_X509_BAD_SIGNATURE:    return "X509_BAD_SIGNATURE";
    case BR_ERR_X509_TIME_UNKNOWN:     return "X509_TIME_UNKNOWN";
    case BR_ERR_X509_EXPIRED:          return "X509_EXPIRED";
    case BR_ERR_X509_DN_MISMATCH:      return "X509_DN_MISMATCH";
    case BR_ERR_X509_BAD_SERVER_NAME:  return "X509_BAD_SERVER_NAME";
    case BR_ERR_X509_CRITICAL_EXTENSION: return "X509_CRITICAL_EXTENSION";
    case BR_ERR_X509_NOT_CA:           return "X509_NOT_CA";
    case BR_ERR_X509_FORBIDDEN_KEY_USAGE: return "X509_FORBIDDEN_KEY_USAGE";
    case BR_ERR_X509_WEAK_PUBLIC_KEY:  return "X509_WEAK_PUBLIC_KEY";
    case BR_ERR_X509_NOT_TRUSTED:      return "X509_NOT_TRUSTED";
    default:                           return "UNKNOWN";
    }
}

/* ---- BearSSL I/O callbacks ---- */

static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = read(fd, buf, len);
    if (n <= 0) return -1;
    return (int)n;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    const unsigned char *p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0) return -1;
        p += n;
        remaining -= (size_t)n;
    }
    return (int)len;
}

/* ---- HTTPS (TLS) fetch ---- */

static int fetch_tls(int fd, const char *host, const char *path, int out_fd) {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;

    /*
     * Using static here keeps ~33KB off the stack. The TLS record
     * layer needs a buffer large enough for one full-size record
     * in each direction.
     */
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];

    br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);

    br_ssl_client_reset(&sc, host, 0);

    /* Seed the engine PRNG with entropy from Stellux's hardware RNG */
    unsigned char seed[32];
    if (getrandom(seed, sizeof(seed), 0) < 0) {
        printf("fetch: getrandom() failed\r\n");
        close(fd);
        return 1;
    }
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof(seed));

    /* Set up the simplified I/O wrapper around the TCP socket */
    br_sslio_context ioc;
    br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);

    /* Build and send the HTTP request through TLS */
    char req[HTTP_REQ_SIZE];
    int req_len = build_http_request(req, sizeof(req), host, path);
    if (req_len < 0 || req_len >= (int)sizeof(req)) {
        printf("fetch: request too large\r\n");
        close(fd);
        return 1;
    }

    if (br_sslio_write_all(&ioc, req, (size_t)req_len) < 0) {
        int err = br_ssl_engine_last_error(&sc.eng);
        printf("fetch: TLS write failed (err=%d: %s)\r\n",
               err, tls_error_name(err));
        close(fd);
        return 1;
    }

    if (br_sslio_flush(&ioc) < 0) {
        int err = br_ssl_engine_last_error(&sc.eng);
        printf("fetch: TLS flush failed (err=%d: %s)\r\n",
               err, tls_error_name(err));
        close(fd);
        return 1;
    }

    /* Read the HTTP response through TLS */
    char buf[FETCH_BUF_SIZE];
    int n;
    size_t total = 0;

    if (out_fd >= 0) {
        /* Save mode: separate headers from body */
        http_split_state hs = {0, 0};
        while ((n = br_sslio_read(&ioc, buf, sizeof(buf))) > 0) {
            ssize_t body = process_response_chunk(&hs, out_fd, buf, (size_t)n);
            if (body < 0) {
                printf("\r\nfetch: write error\r\n");
                close(fd);
                return 1;
            }
            total += (size_t)body;
        }
    } else {
        /* Stdout mode: dump everything (existing behavior) */
        while ((n = br_sslio_read(&ioc, buf, sizeof(buf))) > 0) {
            write(STDOUT_FILENO, buf, (size_t)n);
            total += (size_t)n;
        }
    }

    /* Check for TLS-level errors (as opposed to normal close) */
    int err = br_ssl_engine_last_error(&sc.eng);
    if (err != BR_ERR_OK && err != BR_ERR_IO) {
        printf("\r\nfetch: TLS error (err=%d: %s)\r\n",
               err, tls_error_name(err));
        close(fd);
        return 1;
    }

    if (total == 0 && out_fd < 0)
        printf("fetch: empty response\r\n");

    if (out_fd >= 0)
        printf("\r\n--- %zu body bytes saved ---\r\n", total);
    else
        printf("\r\n--- %zu bytes received (TLS) ---\r\n", total);

    br_sslio_close(&ioc);
    close(fd);
    return 0;
}

/* ---- URL parsing ---- */

struct fetch_params {
    char host[256];
    char path[1024];
    char output_file[512];  /* empty = stdout */
    uint16_t port;
    int use_tls;
    int save_mode;          /* 1 = save body to file */
};

/*
 * Derive a filename from the URL path.
 * "/path/to/file.txt" -> "file.txt"
 * "/" or "/path/to/dir/" -> "index.html"
 */
static void derive_filename(const char *url_path, char *out, size_t out_size) {
    const char *last_slash = strrchr(url_path, '/');
    const char *name = NULL;
    if (last_slash && last_slash[1] != '\0') {
        name = last_slash + 1;
    }
    if (!name || name[0] == '\0') {
        name = "index.html";
    }
    snprintf(out, out_size, "%s", name);
}

static int parse_url(int url_argc, char *url_argv[], struct fetch_params *out) {
    memset(out, 0, sizeof(*out));

    if (url_argc < 1)
        return -1;

    const char *arg = url_argv[0];
    int scheme_explicit = 0;

    /* Strip scheme prefix if present */
    if (strncmp(arg, "https://", 8) == 0) {
        arg += 8;
        out->port = DEFAULT_HTTPS_PORT;
        out->use_tls = 1;
        scheme_explicit = 1;
    } else if (strncmp(arg, "http://", 7) == 0) {
        arg += 7;
        out->port = DEFAULT_HTTP_PORT;
        out->use_tls = 0;
        scheme_explicit = 1;
    } else {
        out->port = DEFAULT_HTTPS_PORT;
        out->use_tls = 1;
    }

    /* Split host and path on first '/' */
    const char *slash = strchr(arg, '/');
    if (slash) {
        size_t host_len = (size_t)(slash - arg);
        if (host_len == 0 || host_len >= sizeof(out->host)) return -1;
        memcpy(out->host, arg, host_len);
        out->host[host_len] = '\0';
        snprintf(out->path, sizeof(out->path), "%s", slash);
    } else {
        if (strlen(arg) == 0 || strlen(arg) >= sizeof(out->host)) return -1;
        snprintf(out->host, sizeof(out->host), "%s", arg);
        snprintf(out->path, sizeof(out->path), "%s", DEFAULT_PATH);
    }

    /* Legacy positional syntax: fetch <host> <port> [path] */
    if (url_argc >= 2 && !scheme_explicit) {
        out->port = (uint16_t)atoi(url_argv[1]);
        if (out->port == 0) {
            printf("fetch: invalid port '%s'\r\n", url_argv[1]);
            return -1;
        }
        out->use_tls = (out->port == DEFAULT_HTTPS_PORT) ? 1 : 0;
    }
    if (url_argc >= 3 && !scheme_explicit) {
        snprintf(out->path, sizeof(out->path), "%s", url_argv[2]);
    }

    /* Remove trailing slash from empty paths */
    if (out->path[0] == '\0')
        snprintf(out->path, sizeof(out->path), "%s", DEFAULT_PATH);

    return 0;
}

static void print_usage(void) {
    printf("Usage: fetch [options] <url>\r\n");
    printf("       fetch [options] <host> [port] [path]\r\n");
    printf("\r\n");
    printf("Options:\r\n");
    printf("  -o <file>    Save response body to file\r\n");
    printf("  --save       Save response body (auto-derive filename from URL)\r\n");
    printf("\r\n");
    printf("Examples:\r\n");
    printf("  fetch google.com                       HTTPS to stdout\r\n");
    printf("  fetch https://example.com/page         HTTPS with path\r\n");
    printf("  fetch http://example.com               HTTP explicit\r\n");
    printf("  fetch https://example.com -o page.html Save body to page.html\r\n");
    printf("  fetch https://example.com/f.tar --save Save body to f.tar\r\n");
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Pre-scan argv for flags, collect remaining args for URL parsing */
    char *url_args[8];
    int url_argc = 0;
    char output_file[512] = {0};
    int save_mode = 0;
    int auto_name = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                printf("fetch: -o requires a filename\r\n");
                return 1;
            }
            save_mode = 1;
            snprintf(output_file, sizeof(output_file), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--save") == 0) {
            save_mode = 1;
            auto_name = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else {
            if (url_argc < 8) url_args[url_argc++] = argv[i];
        }
    }

    if (url_argc == 0) {
        print_usage();
        return 1;
    }

    struct fetch_params params;
    if (parse_url(url_argc, url_args, &params) < 0) {
        print_usage();
        return 1;
    }

    /* Handle --save auto filename derivation */
    if (save_mode) {
        if (auto_name) {
            derive_filename(params.path, output_file, sizeof(output_file));
        }
        snprintf(params.output_file, sizeof(params.output_file), "%s", output_file);
        params.save_mode = 1;
    }

    printf("fetch: %s://%s:%u%s\r\n",
           params.use_tls ? "https" : "http",
           params.host, params.port, params.path);

    if (params.save_mode) {
        printf("fetch: saving to %s\r\n", params.output_file);
    }

    /* Resolve hostname */
    uint32_t ip;
    if (is_ip_address(params.host)) {
        ip = parse_ipv4(params.host);
        if (ip == 0) {
            printf("fetch: invalid IP address '%s'\r\n", params.host);
            return 1;
        }
    } else {
        printf("fetch: resolving %s...\r\n", params.host);
        ip = dns_resolve(params.host);
        if (ip == 0) {
            printf("fetch: failed to resolve '%s'\r\n", params.host);
            return 1;
        }
        char ip_str[16];
        format_ip(ip, ip_str, sizeof(ip_str));
        printf("fetch: resolved to %s\r\n", ip_str);
    }

    /* Open TCP connection */
    int sock_fd = tcp_connect(ip, params.port);
    if (sock_fd < 0)
        return 1;

    /* Open output file if saving */
    int out_fd = -1;
    if (params.save_mode) {
        out_fd = open(params.output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            printf("fetch: cannot open '%s' for writing (errno=%d)\r\n",
                   params.output_file, errno);
            close(sock_fd);
            return 1;
        }
    }

    int rc;
    if (params.use_tls)
        rc = fetch_tls(sock_fd, params.host, params.path, out_fd);
    else
        rc = fetch_plain(sock_fd, params.host, params.path, out_fd);

    if (out_fd >= 0) {
        close(out_fd);
        if (rc == 0)
            printf("fetch: saved to %s\r\n", params.output_file);
    }

    return rc;
}
