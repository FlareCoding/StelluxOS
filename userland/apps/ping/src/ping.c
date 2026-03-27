#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

// ICMP header structure
struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
};

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0
#define ICMP_PAYLOAD_LEN  56
#define ICMP_PACKET_LEN   (sizeof(struct icmp_hdr) + ICMP_PAYLOAD_LEN)

// Receive timeout: poll interval and total wait time per ping
#define PING_RECV_POLL_MS    50       // poll every 50ms
#define PING_RECV_TIMEOUT_MS 3000     // give up after 3 seconds

static uint16_t inet_checksum(const void* data, size_t len) {
    const uint8_t* ptr = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1) {
        uint16_t word = (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
        sum += word;
        ptr += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint16_t)ptr[0];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

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

static void format_ip(uint32_t ip_host, char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "%u.%u.%u.%u",
             (ip_host >> 24) & 0xFF, (ip_host >> 16) & 0xFF,
             (ip_host >>  8) & 0xFF, ip_host & 0xFF);
}

static uint32_t my_htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v & 0xFF0000) >> 8) | ((v & 0xFF000000) >> 24);
}

static uint16_t my_htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static uint16_t my_ntohs(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

#define DNS_SERVER_IP     0x08080808  // 8.8.8.8 (Google Public DNS)
#define DNS_PORT          53
#define DNS_HEADER_LEN    12
#define DNS_TYPE_A        1
#define DNS_CLASS_IN      1
#define DNS_FLAG_RD       0x0100      // Recursion Desired
#define DNS_FLAG_QR       0x8000      // Response flag
#define DNS_RCODE_MASK    0x000F
#define DNS_MAX_PACKET    512
#define DNS_RECV_TIMEOUT_MS  100
#define DNS_RECV_RETRIES     50       // 50 * 100ms = 5s per attempt
#define DNS_SEND_ATTEMPTS    2

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

static uint32_t dns_resolve(const char* hostname) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    // Build DNS query
    uint8_t query[DNS_MAX_PACKET];
    memset(query, 0, sizeof(query));

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint16_t txn_id = (uint16_t)(ts.tv_nsec & 0xFFFF);

    // Header
    query[0] = (uint8_t)(txn_id >> 8);
    query[1] = (uint8_t)(txn_id & 0xFF);
    query[2] = (uint8_t)(DNS_FLAG_RD >> 8);
    query[3] = (uint8_t)(DNS_FLAG_RD & 0xFF);
    query[4] = 0; query[5] = 1; // qdcount = 1

    // Question: QNAME
    int name_len = dns_encode_name(hostname, query + DNS_HEADER_LEN,
                                   sizeof(query) - DNS_HEADER_LEN - 4);
    if (name_len < 0) { close(fd); return 0; }

    size_t q_end = (size_t)(DNS_HEADER_LEN + name_len);
    // QTYPE = A (1), QCLASS = IN (1)
    query[q_end]     = 0; query[q_end + 1] = DNS_TYPE_A;
    query[q_end + 2] = 0; query[q_end + 3] = DNS_CLASS_IN;
    size_t query_len = q_end + 4;

    struct sockaddr_in dns_addr;
    memset(&dns_addr, 0, sizeof(dns_addr));
    dns_addr.sin_family = AF_INET;
    dns_addr.sin_port = my_htons(DNS_PORT);
    dns_addr.sin_addr.s_addr = my_htonl(DNS_SERVER_IP);

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
                struct timespec delay = { .tv_sec = 0,
                                          .tv_nsec = DNS_RECV_TIMEOUT_MS * 1000000L };
                nanosleep(&delay, NULL);
                continue;
            }

            // Verify transaction ID
            uint16_t resp_id = ((uint16_t)resp[0] << 8) | resp[1];
            if (resp_id != txn_id) continue;

            // Check flags: must be a response with no error
            uint16_t flags = ((uint16_t)resp[2] << 8) | resp[3];
            if (!(flags & DNS_FLAG_QR)) continue;
            if ((flags & DNS_RCODE_MASK) != 0) break;

            uint16_t ancount = ((uint16_t)resp[6] << 8) | resp[7];
            if (ancount == 0) break;

            // Skip question section
            int pos = dns_skip_name(resp, (size_t)nrecv, DNS_HEADER_LEN);
            if (pos < 0) break;
            pos += 4; // skip QTYPE + QCLASS

            // Parse answer RRs looking for type A
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

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("Usage: ping <host> [count]\r\n");
        return 1;
    }

    const char* target = argv[1];
    uint32_t dst_ip_host = parse_ipv4(target);
    if (dst_ip_host == 0) {
        dst_ip_host = dns_resolve(target);
        if (dst_ip_host == 0) {
            printf("ping: cannot resolve '%s'\r\n", target);
            return 1;
        }
    }

    int count = 4;
    if (argc >= 3) {
        count = atoi(argv[2]);
        if (count <= 0 || count > 100) {
            printf("ping: invalid count (must be 1-100)\r\n");
            return 1;
        }
    }

    // Open an ICMP datagram socket
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) {
        printf("ping: socket() failed (errno=%d)\r\n", errno);
        return 1;
    }

    char ip_str[32];
    format_ip(dst_ip_host, ip_str, sizeof(ip_str));
    if (parse_ipv4(target) != 0) {
        printf("PING %s: %d data bytes\r\n", ip_str, ICMP_PAYLOAD_LEN);
    } else {
        printf("PING %s (%s): %d data bytes\r\n", target, ip_str, ICMP_PAYLOAD_LEN);
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = my_htonl(dst_ip_host);

    int sent = 0;
    int received = 0;
    uint32_t rtt_min = 0xFFFFFFFF;
    uint32_t rtt_max = 0;
    uint64_t rtt_total = 0;
    uint16_t ping_id = (uint16_t)(uintptr_t)&fd; // unique-ish per process

    for (int i = 0; i < count; i++) {
        // Build ICMP echo request
        uint8_t packet[ICMP_PACKET_LEN];
        memset(packet, 0, sizeof(packet));

        struct icmp_hdr* hdr = (struct icmp_hdr*)packet;
        hdr->type = ICMP_ECHO_REQUEST;
        hdr->code = 0;
        hdr->id = my_htons(ping_id);
        hdr->seq = my_htons((uint16_t)i);
        hdr->checksum = 0;

        // Fill payload with pattern
        for (int j = 0; j < ICMP_PAYLOAD_LEN; j++) {
            packet[sizeof(struct icmp_hdr) + j] = (uint8_t)(j & 0xFF);
        }

        // Compute ICMP checksum
        hdr->checksum = inet_checksum(packet, sizeof(packet));

        // Record send time
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        // Send
        ssize_t nsent = sendto(fd, packet, sizeof(packet), 0,
                               (struct sockaddr*)&dst, sizeof(dst));
        sent++;

        if (nsent < 0) {
            printf("ping: sendto failed (errno=%d)\r\n", errno);
            if (i < count - 1) {
                struct timespec delay = { .tv_sec = 1, .tv_nsec = 0 };
                nanosleep(&delay, NULL);
            }
            continue;
        }

        // Wait for reply using non-blocking poll/sleep loop with timeout.
        // This avoids blocking forever when replies never arrive (e.g.
        // unreachable host, firewall, QEMU SLIRP ICMP limitation).
        uint8_t reply_buf[256];
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        ssize_t nrecv = -1;

        int elapsed_ms = 0;
        while (elapsed_ms < PING_RECV_TIMEOUT_MS) {
            srclen = sizeof(src);
            nrecv = recvfrom(fd, reply_buf, sizeof(reply_buf), MSG_DONTWAIT,
                             (struct sockaddr*)&src, &srclen);
            if (nrecv >= (ssize_t)sizeof(struct icmp_hdr)) {
                break;  // got a reply
            }
            // No data yet — sleep briefly and retry
            struct timespec poll_delay = { .tv_sec = 0,
                                           .tv_nsec = PING_RECV_POLL_MS * 1000000L };
            nanosleep(&poll_delay, NULL);
            elapsed_ms += PING_RECV_POLL_MS;
            nrecv = -1;  // ensure timeout path is taken if loop exhausts
        }

        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);

        if (nrecv < (ssize_t)sizeof(struct icmp_hdr)) {
            printf("Request timeout for icmp_seq=%d\r\n", i);
        } else {
            struct icmp_hdr* reply_hdr = (struct icmp_hdr*)reply_buf;

            if (reply_hdr->type == ICMP_ECHO_REPLY &&
                my_ntohs(reply_hdr->id) == ping_id &&
                my_ntohs(reply_hdr->seq) == (uint16_t)i) {

                long sec_diff = t1.tv_sec - t0.tv_sec;
                long nsec_diff = t1.tv_nsec - t0.tv_nsec;
                if (nsec_diff < 0) {
                    sec_diff--;
                    nsec_diff += 1000000000L;
                }
                uint64_t rtt_ns = (uint64_t)sec_diff * 1000000000ULL + (uint64_t)nsec_diff;
                uint32_t rtt_us = (uint32_t)(rtt_ns / 1000);

                received++;
                rtt_total += rtt_us;
                if (rtt_us < rtt_min) rtt_min = rtt_us;
                if (rtt_us > rtt_max) rtt_max = rtt_us;

                uint32_t ms = rtt_us / 1000;
                uint32_t us_frac = rtt_us % 1000;
                printf("64 bytes from %s: icmp_seq=%d time=%u.%03u ms\r\n",
                       ip_str, i, ms, us_frac);
            } else {
                printf("Request timeout for icmp_seq=%d\r\n", i);
            }
        }

        if (i < count - 1) {
            struct timespec delay = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&delay, NULL);
        }
    }

    printf("--- %s ping statistics ---\r\n", ip_str);
    int loss = (sent > 0) ? ((sent - received) * 100) / sent : 0;
    printf("%d packets transmitted, %d received, %d%% packet loss\r\n",
           sent, received, loss);

    if (received > 0) {
        uint32_t avg = (uint32_t)(rtt_total / (uint64_t)received);
        printf("rtt min/avg/max = %u.%03u/%u.%03u/%u.%03u ms\r\n",
               rtt_min / 1000, rtt_min % 1000,
               avg / 1000, avg % 1000,
               rtt_max / 1000, rtt_max % 1000);
    }

    close(fd);
    return (received > 0) ? 0 : 1;
}
