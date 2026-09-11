#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>

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

// How long to wait for each reply before reporting it lost
#define PING_RECV_TIMEOUT_MS 3000

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

static uint16_t my_htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static uint16_t my_ntohs(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static int resolve_ipv4(const char* host, struct in_addr* out) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* result = NULL;
    if (getaddrinfo(host, NULL, &hints, &result) != 0) {
        return -1;
    }

    *out = ((struct sockaddr_in*)result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return 0;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("Usage: ping <host> [count]\r\n");
        return 1;
    }

    const char* target = argv[1];
    struct in_addr dst_addr;
    if (resolve_ipv4(target, &dst_addr) != 0) {
        printf("ping: cannot resolve '%s'\r\n", target);
        return 1;
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

    // The kernel stamps its own identifier on every request and reports it as the
    // local port, the stack address is only a fallback where that query fails.
    uint16_t ping_id = (uint16_t)(uintptr_t)&fd;
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);

    if (getsockname(fd, (struct sockaddr*)&local, &local_len) == 0 && local_len == sizeof(local)) {
        ping_id = my_ntohs(local.sin_port);
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dst_addr, ip_str, sizeof(ip_str));
    if (strcmp(target, ip_str) == 0) {
        printf("PING %s: %d data bytes\r\n", ip_str, ICMP_PAYLOAD_LEN);
    } else {
        printf("PING %s (%s): %d data bytes\r\n", target, ip_str, ICMP_PAYLOAD_LEN);
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr = dst_addr;

    int sent = 0;
    int received = 0;
    uint32_t rtt_min = 0xFFFFFFFF;
    uint32_t rtt_max = 0;
    uint64_t rtt_total = 0;

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

        hdr->checksum = inet_checksum(packet, sizeof(packet));

        // Record send time
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

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

        // Wait for the matching reply, bounded because it may never arrive
        // (unreachable host, firewall). Anything else on the socket, such as
        // a late reply to an earlier request, is read and skipped.
        uint8_t reply_buf[256];
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        ssize_t nrecv = -1;

        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        for (;;) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long waited_ms = (now.tv_sec - t0.tv_sec) * 1000L +
                             (now.tv_nsec - t0.tv_nsec) / 1000000L;
            if (waited_ms >= PING_RECV_TIMEOUT_MS) {
                break;
            }

            if (poll(&pfd, 1, PING_RECV_TIMEOUT_MS - (int)waited_ms) <= 0) {
                break;
            }

            srclen = sizeof(src);
            nrecv = recvfrom(fd, reply_buf, sizeof(reply_buf), MSG_DONTWAIT,
                             (struct sockaddr*)&src, &srclen);
            if (nrecv >= (ssize_t)sizeof(struct icmp_hdr)) {
                struct icmp_hdr* reply_hdr = (struct icmp_hdr*)reply_buf;
                if (reply_hdr->type == ICMP_ECHO_REPLY &&
                    my_ntohs(reply_hdr->id) == ping_id &&
                    my_ntohs(reply_hdr->seq) == (uint16_t)i) {
                    break;
                }
            }

            nrecv = -1;
        }

        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);

        if (nrecv < 0) {
            printf("Request timeout for icmp_seq=%d\r\n", i);
        } else {
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
