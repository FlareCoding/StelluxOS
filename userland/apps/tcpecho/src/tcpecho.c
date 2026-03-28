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
#include <errno.h>

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

static int run_server(uint16_t port) {
    printf("tcpecho: creating SOCK_STREAM socket...\r\n");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("tcpecho: socket() failed (errno=%d)\r\n", errno);
        return 1;
    }
    printf("tcpecho: socket created (fd=%d)\r\n", fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = 0;

    printf("tcpecho: binding to 0.0.0.0:%u...\r\n", port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("tcpecho: bind() failed (errno=%d)\r\n", errno);
        close(fd);
        return 1;
    }
    printf("tcpecho: bind OK\r\n");

    printf("tcpecho: calling listen()...\r\n");
    if (listen(fd, 5) < 0) {
        printf("tcpecho: listen() failed (errno=%d)\r\n", errno);
        close(fd);
        return 1;
    }
    printf("tcpecho: listening on port %u\r\n", port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        printf("tcpecho: waiting for connection...\r\n");
        int client_fd = accept(fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            printf("tcpecho: accept() failed (errno=%d)\r\n", errno);
            continue;
        }

        uint32_t client_ip = ntohl(client_addr.sin_addr.s_addr);
        uint16_t client_port = ntohs(client_addr.sin_port);
        char ip_str[16];
        format_ip(client_ip, ip_str, sizeof(ip_str));
        printf("tcpecho: connection from %s:%u (fd=%d)\r\n",
               ip_str, client_port, client_fd);

        char buf[1024];
        ssize_t n;
        while ((n = read(client_fd, buf, sizeof(buf))) > 0) {
            buf[n < (ssize_t)sizeof(buf) ? n : (ssize_t)sizeof(buf) - 1] = '\0';
            printf("  recv %zd bytes: \"%s\"\r\n", n, buf);
            write(client_fd, buf, (size_t)n);
        }

        printf("tcpecho: connection closed\r\n");
        close(client_fd);
    }

    close(fd);
    return 0;
}

static int run_client(uint32_t dst_ip_host, uint16_t port, const char* msg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("tcpecho: socket() failed (errno=%d)\r\n", errno);
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = htonl(dst_ip_host);

    char ip_str[16];
    format_ip(dst_ip_host, ip_str, sizeof(ip_str));
    printf("tcpecho: connecting to %s:%u...\r\n", ip_str, port);

    if (connect(fd, (struct sockaddr*)&dst, sizeof(dst)) < 0) {
        printf("tcpecho: connect() failed (errno=%d)\r\n", errno);
        close(fd);
        return 1;
    }
    printf("tcpecho: connected\r\n");

    size_t msg_len = strlen(msg);
    printf("tcpecho: sending \"%s\" (%zu bytes)\r\n", msg, msg_len);
    ssize_t nw = write(fd, msg, msg_len);
    if (nw < 0) {
        printf("tcpecho: write() failed (errno=%d)\r\n", errno);
        close(fd);
        return 1;
    }

    char buf[1024];
    ssize_t nr = read(fd, buf, sizeof(buf) - 1);
    if (nr > 0) {
        buf[nr] = '\0';
        printf("tcpecho: received \"%s\" (%zd bytes)\r\n", buf, nr);
    } else if (nr == 0) {
        printf("tcpecho: connection closed by peer\r\n");
    } else {
        printf("tcpecho: read() failed (errno=%d)\r\n", errno);
    }

    close(fd);
    return 0;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 3) {
        printf("Usage:\r\n");
        printf("  tcpecho server <port>\r\n");
        printf("  tcpecho client <ip> <port> [message]\r\n");
        return 1;
    }

    if (strcmp(argv[1], "server") == 0) {
        uint16_t port = (uint16_t)atoi(argv[2]);
        if (port == 0) {
            printf("tcpecho: invalid port\r\n");
            return 1;
        }
        return run_server(port);
    }

    if (strcmp(argv[1], "client") == 0) {
        if (argc < 4) {
            printf("Usage: tcpecho client <ip> <port> [message]\r\n");
            return 1;
        }
        uint32_t ip = parse_ipv4(argv[2]);
        if (ip == 0) {
            printf("tcpecho: invalid IP address '%s'\r\n", argv[2]);
            return 1;
        }
        uint16_t port = (uint16_t)atoi(argv[3]);
        if (port == 0) {
            printf("tcpecho: invalid port\r\n");
            return 1;
        }
        const char* msg = (argc >= 5) ? argv[4] : "hello";
        return run_client(ip, port, msg);
    }

    printf("tcpecho: unknown mode '%s'\r\n", argv[1]);
    return 1;
}
