/* Requests over http:// to a server on the loopback: the body length a reply states, -1 without one and the final
 * hop's after a redirect, and reads that end at that length or fail as truncated before it */
#define _POSIX_C_SOURCE 200809L

#include <stlxhttp/http.h>

#include <bearssl/bearssl.h>
#include <bearssl/trust_anchors.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_REPORTS 8
#define LOOPBACK    "127.0.0.1"
#define REQUEST_MAX 1024
#define BODY_MAX    256
#define URL_SIZE    128
#define PATH_SIZE   64

typedef struct {
    const char* path;
    const char* reply;
} route;

typedef struct {
    int32_t opened;
    int64_t length;
    int64_t last_read;
    char    body[BODY_MAX + 1];
} fetched;

static const route ROUTES[] = {
    { "/length", "HTTP/1.0 200 OK\r\nContent-Length: 11\r\n\r\nhello world" },
    { "/unstated", "HTTP/1.0 200 OK\r\n\r\nuntil the close" },
    { "/redirect", "HTTP/1.0 302 Found\r\nLocation: /length\r\nContent-Length: 0\r\n\r\n" },
    { "/short", "HTTP/1.0 200 OK\r\nContent-Length: 100\r\n\r\ncut short" },
};

static uint64_t g_checks;
static uint64_t g_failures;
static uint16_t g_port;

/* http.c's trust anchors and TLS calls, which no http:// request reaches */
const br_x509_trust_anchor TAs[1] = { 0 };
const size_t               TAs_NUM = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

void br_ssl_client_init_full(br_ssl_client_context* client, br_x509_minimal_context* x509,
                             const br_x509_trust_anchor* anchors, size_t anchor_count) {
    abort();
}

void br_ssl_engine_set_buffer(br_ssl_engine_context* engine, void* buffer, size_t length, int is_bidirectional) {
    abort();
}

void br_ssl_engine_inject_entropy(br_ssl_engine_context* engine, const void* bytes, size_t length) {
    abort();
}

int br_ssl_client_reset(br_ssl_client_context* client, const char* server_name, int resumes_session) {
    abort();
}

void br_sslio_init(br_sslio_context* io, br_ssl_engine_context* engine,
                   int (*low_read)(void* context, unsigned char* bytes, size_t length), void* read_context,
                   int (*low_write)(void* context, const unsigned char* bytes, size_t length), void* write_context) {
    abort();
}

int br_sslio_read(br_sslio_context* io, void* bytes, size_t length) {
    abort();
}

int br_sslio_write_all(br_sslio_context* io, const void* bytes, size_t length) {
    abort();
}

int br_sslio_flush(br_sslio_context* io) {
    abort();
}

#pragma GCC diagnostic pop

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static const char* reply_for(const char* request) {
    char path[PATH_SIZE] = "";
    sscanf(request, "GET %63s", path);
    for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); i++) {
        if (strcmp(ROUTES[i].path, path) == 0) {
            return ROUTES[i].reply;
        }
    }

    return "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
}

/* Answers each connection by its request's path, then closes it */
static void serve(int listener) {
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            continue;
        }

        char request[REQUEST_MAX];
        size_t length = 0;
        request[0] = '\0';
        while (length < sizeof(request) - 1 && !strstr(request, "\r\n\r\n")) {
            ssize_t count = read(fd, request + length, sizeof(request) - 1 - length);
            if (count <= 0) {
                break;
            }

            length += (size_t)count;
            request[length] = '\0';
        }

        const char* reply = reply_for(request);
        if (write(fd, reply, strlen(reply)) < 0) {
            perror("http: write");
        }

        close(fd);
    }
}

static pid_t start_server(void) {
    struct sockaddr_in address = { .sin_family = AF_INET };
    socklen_t size = sizeof(address);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || inet_pton(AF_INET, LOOPBACK, &address.sin_addr) != 1 ||
        bind(listener, (struct sockaddr*)&address, size) != 0 || listen(listener, 8) != 0 ||
        getsockname(listener, (struct sockaddr*)&address, &size) != 0) {
        return -1;
    }

    g_port = ntohs(address.sin_port);
    pid_t server = fork();
    if (server == 0) {
        serve(listener);
    }

    close(listener);
    return server;
}

/* Opens `path` on the server and reads its body until a read returns 0 or fails */
static void fetch(const char* path, uint32_t flags, fetched* result) {
    char url[URL_SIZE];
    snprintf(url, sizeof(url), "http://" LOOPBACK ":%u%s", (unsigned)g_port, path);

    stlxhttp_request* request = NULL;
    result->opened = stlxhttp_open(url, flags, &request);
    result->length = result->opened == STLXHTTP_OK ? stlxhttp_content_length(request) : 0;
    result->last_read = 0;

    size_t received = 0;
    int64_t count = result->opened == STLXHTTP_OK ? 1 : 0;
    while (count > 0 && received < BODY_MAX) {
        count = stlxhttp_read(request, result->body + received, BODY_MAX - received);
        received += count > 0 ? (size_t)count : 0;
        result->last_read = count;
    }

    result->body[received] = '\0';
    stlxhttp_close(request);
}

static void check_lengths(void) {
    fetched result;
    fetch("/length", 0, &result);
    check(result.opened == STLXHTTP_OK && result.length == 11, "a stated length is the reply's");
    check(strcmp(result.body, "hello world") == 0 && result.last_read == 0, "the body ends at its stated length");

    fetch("/unstated", 0, &result);
    check(result.opened == STLXHTTP_OK && result.length == -1, "a reply stating no length has length -1");
    check(strcmp(result.body, "until the close") == 0 && result.last_read == 0, "such a body ends at the close");

    fetch("/redirect", 0, &result);
    check(result.opened == STLXHTTP_OK && result.length == 11, "a followed redirect has the final reply's length");
    check(strcmp(result.body, "hello world") == 0, "a followed redirect reads the final reply's body");

    fetch("/redirect", STLXHTTP_NO_FOLLOW, &result);
    check(result.opened == STLXHTTP_OK && result.length == 0, "an unfollowed redirect has its own length");

    fetch("/short", 0, &result);
    check(result.opened == STLXHTTP_OK && result.length == 100, "a short reply keeps the length it stated");
    check(strcmp(result.body, "cut short") == 0 && result.last_read == STLXHTTP_ERR_TRUNCATED,
          "a body ending before its stated length fails as truncated after what arrived");
}

int main(void) {
    pid_t server = start_server();
    if (server < 0) {
        printf("http: cannot start a server on the loopback\n");
        return 1;
    }

    check_lengths();

    kill(server, SIGTERM);
    waitpid(server, NULL, 0);

    if (g_failures != 0) {
        printf("http: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("http: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
