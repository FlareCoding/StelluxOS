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

#define MAX_BUF 1024
#define RECV_POLL_MS 100
#define RECV_TIMEOUT_MS 5000

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

static uint32_t my_ntohl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v & 0xFF0000) >> 8) | ((v & 0xFF000000) >> 24);
}

static int run_server(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("udpecho: socket() failed (errno=%d)\r\n", errno);
        return 1;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = my_htons(port);
    bind_addr.sin_addr.s_addr = 0;

    if (bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        printf("udpecho: bind() failed (errno=%d)\r\n", errno);
        close(fd);
        return 1;
    }

    printf("udpecho: listening on 0.0.0.0:%u\r\n", port);

    uint8_t buf[MAX_BUF];
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr*)&from, &fromlen);
        if (n <= 0) {
            struct timespec delay = { .tv_sec = 0,
                                      .tv_nsec = RECV_POLL_MS * 1000000L };
            nanosleep(&delay, NULL);
            continue;
        }

        char ip_str[16];
        uint32_t src_ip = my_ntohl(from.sin_addr.s_addr);
        uint16_t src_port = my_ntohs(from.sin_port);
        format_ip(src_ip, ip_str, sizeof(ip_str));

        buf[n < MAX_BUF ? n : MAX_BUF - 1] = '\0';
        printf("  %s:%u -> \"%s\" (%zd bytes)\r\n", ip_str, src_port,
               (char*)buf, n);

        ssize_t sent = sendto(fd, buf, (size_t)n, 0,
                              (struct sockaddr*)&from, fromlen);
        if (sent < 0) {
            printf("  echo failed (errno=%d)\r\n", errno);
        }
    }

    close(fd);
    return 0;
}

static int run_client(uint32_t dst_ip_host, uint16_t port, const char* msg) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("udpecho: socket() failed (errno=%d)\r\n", errno);
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = my_htons(port);
    dst.sin_addr.s_addr = my_htonl(dst_ip_host);

    char ip_str[16];
    format_ip(dst_ip_host, ip_str, sizeof(ip_str));

    size_t msg_len = strlen(msg);
    printf("udpecho: sending \"%s\" to %s:%u\r\n", msg, ip_str, port);

    ssize_t sent = sendto(fd, msg, msg_len, 0,
                          (struct sockaddr*)&dst, sizeof(dst));
    if (sent < 0) {
        printf("udpecho: sendto failed (errno=%d)\r\n", errno);
        close(fd);
        return 1;
    }

    uint8_t reply[MAX_BUF];
    int elapsed_ms = 0;
    while (elapsed_ms < RECV_TIMEOUT_MS) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, reply, sizeof(reply), MSG_DONTWAIT,
                             (struct sockaddr*)&from, &fromlen);
        if (n > 0) {
            reply[n < MAX_BUF ? n : MAX_BUF - 1] = '\0';
            printf("udpecho: reply \"%s\" (%zd bytes)\r\n", (char*)reply, n);
            close(fd);
            return 0;
        }
        struct timespec delay = { .tv_sec = 0,
                                  .tv_nsec = RECV_POLL_MS * 1000000L };
        nanosleep(&delay, NULL);
        elapsed_ms += RECV_POLL_MS;
    }

    printf("udpecho: no reply (timeout)\r\n");
    close(fd);
    return 1;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 3) {
        printf("Usage:\r\n");
        printf("  udpecho server <port>\r\n");
        printf("  udpecho client <ip> <port> [message]\r\n");
        return 1;
    }

    if (strcmp(argv[1], "server") == 0) {
        uint16_t port = (uint16_t)atoi(argv[2]);
        if (port == 0) {
            printf("udpecho: invalid port\r\n");
            return 1;
        }
        return run_server(port);
    }

    if (strcmp(argv[1], "client") == 0) {
        if (argc < 4) {
            printf("Usage: udpecho client <ip> <port> [message]\r\n");
            return 1;
        }
        uint32_t ip = parse_ipv4(argv[2]);
        if (ip == 0) {
            printf("udpecho: invalid IP address '%s'\r\n", argv[2]);
            return 1;
        }
        uint16_t port = (uint16_t)atoi(argv[3]);
        if (port == 0) {
            printf("udpecho: invalid port\r\n");
            return 1;
        }
        const char* msg = (argc >= 5) ? argv[4] : "hello";
        return run_client(ip, port, msg);
    }

    printf("udpecho: unknown mode '%s' (use 'server' or 'client')\r\n",
           argv[1]);
    return 1;
}
