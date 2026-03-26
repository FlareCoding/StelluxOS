#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>

// Must match kernel/net/net.h definitions
#define NET_PING       0x4E01
#define NET_GET_CONFIG 0x4E02

struct net_ping_req {
    uint32_t dst_ip;
    uint16_t id;
    uint16_t seq;
    uint32_t timeout_ms;
    int32_t  result;
    uint32_t rtt_us;
} __attribute__((packed));

struct net_config_info {
    uint8_t  mac[6];
    uint8_t  padding[2];
    uint32_t ipv4_addr;
    uint32_t ipv4_netmask;
    uint32_t ipv4_gateway;
    char     name[16];
} __attribute__((packed));

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

    // Return in big-endian order (most significant octet first)
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

static void format_ip(uint32_t ip_net, char* buf, size_t buf_size) {
    // ip_net is in network byte order (big-endian)
    uint8_t a = (ip_net >> 24) & 0xFF;
    uint8_t b = (ip_net >> 16) & 0xFF;
    uint8_t c = (ip_net >>  8) & 0xFF;
    uint8_t d =  ip_net        & 0xFF;
    snprintf(buf, buf_size, "%u.%u.%u.%u", a, b, c, d);
}

// htonl for converting host to network byte order
static uint32_t my_htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v & 0xFF0000) >> 8) | ((v & 0xFF000000) >> 24);
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("Usage: ping <ip_address> [count]\r\n");
        printf("Example: ping 10.0.2.2\r\n");
        printf("         ping 10.0.2.2 5\r\n");
        return 1;
    }

    uint32_t dst_ip = parse_ipv4(argv[1]);
    if (dst_ip == 0) {
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

    int fd = open("/dev/net0", O_RDWR);
    if (fd < 0) {
        printf("ping: cannot open /dev/net0 (no network interface?)\r\n");
        return 1;
    }

    char ip_str[32];
    format_ip(dst_ip, ip_str, sizeof(ip_str));
    printf("PING %s: %d data bytes\r\n", ip_str, 56);

    int sent = 0;
    int received = 0;
    uint32_t rtt_min = 0xFFFFFFFF;
    uint32_t rtt_max = 0;
    uint64_t rtt_total = 0;

    // Convert to network byte order for the kernel
    uint32_t dst_ip_net = my_htonl(dst_ip);

    for (int i = 0; i < count; i++) {
        struct net_ping_req req;
        memset(&req, 0, sizeof(req));
        req.dst_ip = dst_ip_net;
        req.id = (uint16_t)(i + 1);
        req.seq = (uint16_t)i;
        req.timeout_ms = 3000;
        req.result = -1;
        req.rtt_us = 0;

        int rc = ioctl(fd, NET_PING, &req);
        sent++;

        if (rc == 0 && req.result == 0) {
            received++;
            uint32_t rtt = req.rtt_us;
            rtt_total += rtt;
            if (rtt < rtt_min) rtt_min = rtt;
            if (rtt > rtt_max) rtt_max = rtt;

            uint32_t ms = rtt / 1000;
            uint32_t us_frac = rtt % 1000;
            printf("64 bytes from %s: icmp_seq=%d time=%u.%03u ms\r\n",
                   ip_str, i, ms, us_frac);
        } else {
            printf("Request timeout for icmp_seq=%d\r\n", i);
        }

        if (i < count - 1) {
            struct timespec delay = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&delay, NULL);
        }
    }

    printf("--- %s ping statistics ---\r\n", ip_str);
    int loss = 0;
    if (sent > 0) {
        loss = ((sent - received) * 100) / sent;
    }
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
