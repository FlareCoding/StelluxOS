#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define SERVER_PORT 7777
#define LOOPBACK    0x7F000001  /* 127.0.0.1 */
#define POLL_MS     50
#define POLL_RETRIES 60        /* 60 * 50ms = 3s timeout */

static uint16_t my_htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static uint32_t my_htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v & 0xFF0000) >> 8) | ((v & 0xFF000000) >> 24);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("udpecho: UDP loopback echo test\r\n");

    /* Create server socket and bind to port */
    int server = socket(AF_INET, SOCK_DGRAM, 0);
    if (server < 0) {
        printf("FAIL: server socket()\r\n");
        return 1;
    }

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = my_htons(SERVER_PORT);
    srv_addr.sin_addr.s_addr = 0;

    if (bind(server, (struct sockaddr*)&srv_addr, sizeof(srv_addr)) < 0) {
        printf("FAIL: bind() port %d\r\n", SERVER_PORT);
        close(server);
        return 1;
    }
    printf("  server bound to port %d\r\n", SERVER_PORT);

    /* Create client socket */
    int client = socket(AF_INET, SOCK_DGRAM, 0);
    if (client < 0) {
        printf("FAIL: client socket()\r\n");
        close(server);
        return 1;
    }

    /* Client sends to server on loopback */
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = my_htons(SERVER_PORT);
    dst.sin_addr.s_addr = my_htonl(LOOPBACK);

    const char* msg = "hello from udpecho";
    size_t msg_len = strlen(msg);
    ssize_t nsent = sendto(client, msg, msg_len, 0,
                           (struct sockaddr*)&dst, sizeof(dst));
    if (nsent < 0) {
        printf("FAIL: client sendto()\r\n");
        close(client);
        close(server);
        return 1;
    }
    printf("  client sent %zd bytes\r\n", nsent);

    /* Server receives (poll with timeout) */
    char buf[256];
    memset(buf, 0, sizeof(buf));
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t nrecv = -1;

    for (int i = 0; i < POLL_RETRIES; i++) {
        fromlen = sizeof(from);
        nrecv = recvfrom(server, buf, sizeof(buf), MSG_DONTWAIT,
                         (struct sockaddr*)&from, &fromlen);
        if (nrecv > 0) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = POLL_MS * 1000000L };
        nanosleep(&ts, NULL);
    }

    if (nrecv <= 0) {
        printf("FAIL: server recvfrom() timeout\r\n");
        close(client);
        close(server);
        return 1;
    }
    printf("  server received %zd bytes: \"%.*s\"\r\n",
           nrecv, (int)nrecv, buf);

    /* Server echoes back to client */
    nsent = sendto(server, buf, (size_t)nrecv, 0,
                   (struct sockaddr*)&from, fromlen);
    if (nsent < 0) {
        printf("FAIL: server sendto() echo\r\n");
        close(client);
        close(server);
        return 1;
    }

    /* Client receives echo (poll with timeout) */
    char echo[256];
    memset(echo, 0, sizeof(echo));
    nrecv = -1;

    for (int i = 0; i < POLL_RETRIES; i++) {
        nrecv = recvfrom(client, echo, sizeof(echo), MSG_DONTWAIT,
                         NULL, NULL);
        if (nrecv > 0) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = POLL_MS * 1000000L };
        nanosleep(&ts, NULL);
    }

    if (nrecv <= 0) {
        printf("FAIL: client recvfrom() timeout\r\n");
        close(client);
        close(server);
        return 1;
    }

    if (nrecv == (ssize_t)msg_len && memcmp(echo, msg, (size_t)nrecv) == 0) {
        printf("  echo matched (%zd bytes)\r\n", nrecv);
        printf("OK\r\n");
    } else {
        printf("FAIL: echo mismatch\r\n");
        close(client);
        close(server);
        return 1;
    }

    close(client);
    close(server);
    return 0;
}
