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

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("Usage: ping <ip_address> [count]\r\n");
        return 1;
    }

    uint32_t dst_ip_host = parse_ipv4(argv[1]);
    if (dst_ip_host == 0) {
        printf("ping: invalid IP address '%s'\r\n", argv[1]);
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

    char ip_str[32];
    format_ip(dst_ip_host, ip_str, sizeof(ip_str));
    printf("PING %s: %d data bytes\r\n", ip_str, ICMP_PAYLOAD_LEN);

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

        // Wait for reply (blocking recvfrom)
        uint8_t reply_buf[256];
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);

        ssize_t nrecv = recvfrom(fd, reply_buf, sizeof(reply_buf), 0,
                                 (struct sockaddr*)&src, &srclen);

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
