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

        // TODO: read/write echo loop
        printf("tcpecho: closing connection (read/write not yet implemented)\r\n");
        close(client_fd);
    }

    close(fd);
    return 0;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 3) {
        printf("Usage:\r\n");
        printf("  tcpecho server <port>\r\n");
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

    printf("tcpecho: unknown mode '%s'\r\n", argv[1]);
    return 1;
}
