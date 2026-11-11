/* Prints or saves the body of an http or https URL. A saved body is written under a temporary name and takes the
 * output's name only once it arrived whole, so a failed fetch never leaves a file behind. */
#define _POSIX_C_SOURCE 200809L

#include <stlxhttp/http.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BODY_CHUNK        16384
#define HTTPS_PORT        443
#define POSITIONAL_MAX    3
#define TEMPORARY_SUFFIX  ".tmp"
#define DEFAULT_FILE_NAME "index.html"

static char g_body_chunk[BODY_CHUNK];
static char g_url[STLXHTTP_URL_MAX + 1];

static void print_usage(void) {
    printf("Usage: fetch [options] <url>\n"
           "       fetch [options] <host> [port] [path]\n"
           "\n"
           "Options:\n"
           "  -o <file>    Save the body to file\n"
           "  --save       Save the body under the last name in the URL's path\n"
           "\n"
           "Examples:\n"
           "  fetch google.com                       HTTPS to stdout\n"
           "  fetch https://example.com/page         HTTPS with path\n"
           "  fetch http://example.com               HTTP explicit\n"
           "  fetch https://example.com -o page.html Save body to page.html\n"
           "  fetch https://example.com/f.tar --save Save body to f.tar\n");
}

static bool write_all(int fd, const char* bytes, size_t length) {
    while (length > 0) {
        ssize_t count = write(fd, bytes, length);
        if (count <= 0) {
            return false;
        }

        bytes += count;
        length -= (size_t)count;
    }

    return true;
}

/* Builds a URL from `<host>[/path] [port] [path]`, with plain HTTP on any port but 443 */
static const char* build_url(int count, char* args[]) {
    size_t host_length = strcspn(args[0], "/");
    const char* path = args[0][host_length] ? args[0] + host_length : "/";
    long port = HTTPS_PORT;
    if (count > 1) {
        char* end = NULL;
        port = strtol(args[1], &end, 10);
        if (*end != '\0' || port <= 0 || port > USHRT_MAX) {
            fprintf(stderr, "fetch: invalid port '%s'\n", args[1]);
            return NULL;
        }
    }

    if (count > 2) {
        path = args[2];
    }

    const char* scheme = port == HTTPS_PORT ? "https" : "http";
    const char* slash = path[0] == '/' ? "" : "/";
    int length = snprintf(g_url, sizeof(g_url), "%s://%.*s:%ld%s%s", scheme, (int)host_length, args[0], port,
                          slash, path);
    if (length >= (int)sizeof(g_url)) {
        fprintf(stderr, "fetch: the URL is longer than %d characters\n", STLXHTTP_URL_MAX);
        return NULL;
    }

    return g_url;
}

/* The last name in the URL's path, without its query, or index.html for a directory */
static void derive_file_name(const char* url, char* name, size_t size) {
    const char* path = strchr(strstr(url, "://") + 3, '/');
    size_t path_length = path ? strcspn(path, "?#") : 0;
    const char* last = path;
    for (size_t i = 0; i < path_length; i++) {
        if (path[i] == '/') {
            last = path + i + 1;
        }
    }

    int last_length = path ? (int)(path + path_length - last) : 0;
    if (last_length == 0) {
        snprintf(name, size, "%s", DEFAULT_FILE_NAME);
        return;
    }

    snprintf(name, size, "%.*s", last_length, last);
}

/* Copies the body to `fd`, returning its length, or -1 once a failure is printed */
static int64_t copy_body(stlxhttp_request* request, int fd, const char* destination) {
    int64_t total = 0;
    while (true) {
        int64_t count = stlxhttp_read(request, g_body_chunk, sizeof(g_body_chunk));
        if (count == 0) {
            return total;
        }

        if (count < 0) {
            fprintf(stderr, "fetch: %s\n", stlxhttp_error_message(request));
            return -1;
        }

        if (!write_all(fd, g_body_chunk, (size_t)count)) {
            fprintf(stderr, "fetch: cannot write %s (errno %d)\n", destination, errno);
            return -1;
        }

        total += count;
    }
}

static int save_body(stlxhttp_request* request, const char* output) {
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s%s", output, TEMPORARY_SUFFIX) >= (int)sizeof(temporary)) {
        fprintf(stderr, "fetch: the file name %s is too long\n", output);
        return 1;
    }

    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "fetch: cannot create %s (errno %d)\n", temporary, errno);
        return 1;
    }

    int64_t total = copy_body(request, fd, temporary);
    close(fd);
    if (total < 0) {
        unlink(temporary);
        return 1;
    }

    if (rename(temporary, output) != 0) {
        fprintf(stderr, "fetch: cannot rename %s to %s (errno %d)\n", temporary, output, errno);
        unlink(temporary);
        return 1;
    }

    printf("fetch: saved %lld bytes to %s\n", (long long)total, output);

    return 0;
}

int main(int argc, char* argv[]) {
    char* positional[POSITIONAL_MAX];
    int positional_count = 0;
    const char* output = NULL;
    bool save = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0) {
            fprintf(stderr, "fetch: -o requires a file name\n");
            return 1;
        } else if (strcmp(argv[i], "--save") == 0) {
            save = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (positional_count < POSITIONAL_MAX) {
            positional[positional_count++] = argv[i];
        }
    }

    if (positional_count == 0) {
        print_usage();
        return 1;
    }

    const char* url = strstr(positional[0], "://") ? positional[0] : build_url(positional_count, positional);
    if (!url) {
        return 1;
    }

    char derived_name[NAME_MAX + 1];
    if (save && !output) {
        derive_file_name(url, derived_name, sizeof(derived_name));
        output = derived_name;
    }

    stlxhttp_request* request = NULL;
    if (stlxhttp_open(url, 0, &request) != STLXHTTP_OK) {
        fprintf(stderr, "fetch: %s\n", stlxhttp_error_message(request));
        stlxhttp_close(request);
        return 1;
    }

    int rc = 0;
    if (output) {
        rc = save_body(request, output);
    } else if (copy_body(request, STDOUT_FILENO, "stdout") < 0) {
        rc = 1;
    }

    stlxhttp_close(request);

    return rc;
}
