#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stlx/net.h>

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

#define HTTP_BUF_SIZE        4096
#define HTTP_REQ_SIZE        2048
#define DEFAULT_PORT         80
#define DEFAULT_PATH         "/"

static uint32_t parse_ipv4(const char* str) {
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

static int is_ip_address(const char* str) {
    for (int i = 0; str[i]; i++) {
        if (str[i] != '.' && (str[i] < '0' || str[i] > '9')) {
            return 0;
        }
    }
    return 1;
}

static void format_ip(uint32_t ip, char* buf, size_t sz) {
    snprintf(buf, sz, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

/* ---------- DNS resolution (reused from nslookup) ---------- */

static uint32_t dns_htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v & 0xFF0000) >> 8) | ((v & 0xFF000000) >> 24);
}

static uint16_t dns_htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static int dns_encode_name(const char* name, uint8_t* buf, size_t buf_size) {
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 253) return -1;

    size_t pos = 0;
    const char* seg = name;

    while (*seg) {
        const char* dot = seg;
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

static int dns_skip_name(const uint8_t* pkt, size_t pkt_len, size_t offset) {
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

    const struct stlx_ifinfo* def = stlx_net_default_if(&st);
    if (def && def->ipv4_dns != 0) return def->ipv4_dns;
    return DNS_FALLBACK_SERVER;
}

static uint32_t dns_resolve(const char* hostname) {
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
                               (struct sockaddr*)&dns_addr, sizeof(dns_addr));
        if (nsent < 0) continue;

        for (int poll = 0; poll < DNS_RECV_RETRIES; poll++) {
            uint8_t resp[DNS_MAX_PACKET];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t nrecv = recvfrom(fd, resp, sizeof(resp), MSG_DONTWAIT,
                                     (struct sockaddr*)&from, &fromlen);
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

/* ---------- HTTP ---------- */

static ssize_t write_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0) return -1;
        p += n;
        remaining -= (size_t)n;
    }
    return (ssize_t)len;
}

static int http_fetch(uint32_t ip_host, uint16_t port,
                      const char* host, const char* path) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("httpfetch: socket() failed (errno=%d)\r\n", errno);
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = htonl(ip_host);

    char ip_str[16];
    format_ip(ip_host, ip_str, sizeof(ip_str));
    printf("httpfetch: connecting to %s:%u...\r\n", ip_str, port);

    if (connect(fd, (struct sockaddr*)&dst, sizeof(dst)) < 0) {
        printf("httpfetch: connect failed (errno=%d)\r\n", errno);
        close(fd);
        return 1;
    }

    char req[HTTP_REQ_SIZE];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    if (write_all(fd, req, (size_t)req_len) < 0) {
        printf("httpfetch: failed to send request (errno=%d)\r\n", errno);
        close(fd);
        return 1;
    }

    char buf[HTTP_BUF_SIZE];
    ssize_t n;
    size_t total = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, (size_t)n);
        total += (size_t)n;
    }

    if (total == 0) {
        printf("httpfetch: empty response\r\n");
    }

    printf("\r\n--- %zu bytes received ---\r\n", total);
    close(fd);
    return 0;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("Usage: httpfetch <host> [port] [path]\r\n");
        printf("  httpfetch example.com\r\n");
        printf("  httpfetch 93.184.216.34 80 /index.html\r\n");
        return 1;
    }

    const char* host = argv[1];
    uint16_t port = DEFAULT_PORT;
    const char* path = DEFAULT_PATH;

    if (argc >= 3) {
        port = (uint16_t)atoi(argv[2]);
        if (port == 0) {
            printf("httpfetch: invalid port\r\n");
            return 1;
        }
    }
    if (argc >= 4) {
        path = argv[3];
    }

    uint32_t ip;
    if (is_ip_address(host)) {
        ip = parse_ipv4(host);
        if (ip == 0) {
            printf("httpfetch: invalid IP address '%s'\r\n", host);
            return 1;
        }
    } else {
        printf("httpfetch: resolving %s...\r\n", host);
        ip = dns_resolve(host);
        if (ip == 0) {
            printf("httpfetch: failed to resolve '%s'\r\n", host);
            return 1;
        }
        char ip_str[16];
        format_ip(ip, ip_str, sizeof(ip_str));
        printf("httpfetch: resolved to %s\r\n", ip_str);
    }

    return http_fetch(ip, port, host, path);
}
