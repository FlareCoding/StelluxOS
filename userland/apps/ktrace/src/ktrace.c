#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONTROL_PATH "/dev/ktrace/control"
#define STATUS_PATH  "/dev/ktrace/status"
#define TRACE_PATH   "/dev/ktrace/trace"

#define DUMP_CHUNK_BYTES (64 * 1024)

#define CPU_FLAG_RING_WRAPPED 0x1
#define CPU_FLAG_NO_RING      0x2

#define NS_PER_SEC  1000000000ull
#define NS_PER_MSEC 1000000ull

static char g_status[16384];
static char g_dump_buffer[DUMP_CHUNK_BYTES];

static int read_status(void) {
    int fd = open(STATUS_PATH, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    size_t length = 0;
    while (length < sizeof(g_status) - 1) {
        ssize_t n = read(fd, g_status + length, sizeof(g_status) - 1 - length);
        if (n <= 0) {
            break;
        }

        length += (size_t)n;
    }

    close(fd);
    g_status[length] = '\0';
    return 0;
}

static const char* next_line(const char* line) {
    const char* newline = strchr(line, '\n');
    return newline ? newline + 1 : line + strlen(line);
}

static const char* status_field(const char* key) {
    size_t key_length = strlen(key);
    for (const char* line = g_status; *line; line = next_line(line)) {
        if (strncmp(line, key, key_length) == 0 && line[key_length] == ' ') {
            return line + key_length + 1;
        }
    }

    return NULL;
}

static uint64_t status_u64(const char* key) {
    const char* field = status_field(key);
    return field ? strtoull(field, NULL, 10) : 0;
}

static bool status_state_is(const char* state) {
    const char* field = status_field("state");
    size_t length = strlen(state);
    return field && strncmp(field, state, length) == 0 && field[length] == '\n';
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * NS_PER_SEC + (uint64_t)now.tv_nsec;
}

static void print_seconds(uint64_t ns) {
    printf("%llu.%03llu s", (unsigned long long)(ns / NS_PER_SEC),
           (unsigned long long)(ns % NS_PER_SEC / NS_PER_MSEC));
}

static void print_session(void) {
    printf("session       ");
    if (status_state_is("idle")) {
        printf("none yet\n");
        return;
    }

    uint64_t start_ns = status_u64("session_start_ns");
    uint64_t stop_ns = status_u64("session_stop_ns");

    if (stop_ns == 0) {
        print_seconds(monotonic_ns() - start_ns);
        printf(" so far, started at ");
        print_seconds(start_ns);
        printf(" after boot\n");
        return;
    }

    print_seconds(stop_ns - start_ns);
    printf(", from ");
    print_seconds(start_ns);
    printf(" to ");
    print_seconds(stop_ns);
    printf(" after boot\n");
}

static void print_cpus(void) {
    printf("\ncpu  records  flags\n");

    for (const char* line = g_status; *line; line = next_line(line)) {
        if (strncmp(line, "cpu", 3) != 0) {
            continue;
        }

        char* end = NULL;
        unsigned long cpu = strtoul(line + 3, &end, 10);
        unsigned long long kept = strtoull(end, &end, 10);
        unsigned long flags = strtoul(end, &end, 10);

        if (flags == 0) {
            printf("%-4lu %llu\n", cpu, kept);
            continue;
        }

        printf("%-4lu %-8llu %s\n", cpu, kept, (flags & CPU_FLAG_RING_WRAPPED) ? "wrapped" : "no ring");
    }
}

static int show_status(void) {
    if (read_status() != 0) {
        fprintf(stderr, "ktrace: %s: %s\n", STATUS_PATH, strerror(errno));
        return 1;
    }

    const char* state = status_field("state");
    printf("state         %.*s\n", state ? (int)strcspn(state, "\n") : 0, state ? state : "");
    print_session();
    printf("trace open    %llu\n", (unsigned long long)status_u64("trace_open"));
    printf("capacity      %llu records per CPU\n", (unsigned long long)status_u64("capacity"));
    print_cpus();
    return 0;
}

// Returns 0, or the errno value the write failed with
static int send_command(const char* command) {
    int fd = open(CONTROL_PATH, O_WRONLY);
    if (fd < 0) {
        return errno;
    }

    int err = write(fd, command, strlen(command)) < 0 ? errno : 0;
    close(fd);
    return err;
}

static int start_session(void) {
    int err = send_command("start");
    if (err == 0) {
        return 0;
    }

    if (err == EBUSY && read_status() == 0 && status_u64("trace_open") > 0) {
        fprintf(stderr, "ktrace: cannot start: the trace is still open\n");
    } else if (err == EBUSY) {
        fprintf(stderr, "ktrace: cannot start: a session is already recording\n");
    } else {
        fprintf(stderr, "ktrace: cannot start: %s\n", strerror(err));
    }

    return 1;
}

static int stop_session(void) {
    int err = send_command("stop");
    if (err == 0) {
        return 0;
    }

    if (err == EINVAL) {
        fprintf(stderr, "ktrace: cannot stop: no session is recording\n");
    } else {
        fprintf(stderr, "ktrace: cannot stop: %s\n", strerror(err));
    }

    return 1;
}

static bool write_all(int fd, const char* data, size_t length) {
    while (length > 0) {
        ssize_t n = write(fd, data, length);
        if (n < 0) {
            return false;
        }

        data += n;
        length -= (size_t)n;
    }

    return true;
}

static int dump_trace(const char* path) {
    int trace = open(TRACE_PATH, O_RDONLY);
    if (trace < 0) {
        if (errno == ENODATA) {
            fprintf(stderr, "ktrace: cannot dump: no session has been recorded\n");
        } else if (errno == EBUSY) {
            fprintf(stderr, "ktrace: cannot dump: a session is still recording\n");
        } else {
            fprintf(stderr, "ktrace: %s: %s\n", TRACE_PATH, strerror(errno));
        }

        return 1;
    }

    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        fprintf(stderr, "ktrace: %s: %s\n", path, strerror(errno));
        close(trace);
        return 1;
    }

    unsigned long long total = 0;
    int err = 0;
    while (true) {
        ssize_t n = read(trace, g_dump_buffer, sizeof(g_dump_buffer));
        if (n == 0) {
            break;
        }

        if (n < 0 || !write_all(out, g_dump_buffer, (size_t)n)) {
            err = errno;
            break;
        }

        total += (unsigned long long)n;
    }

    close(out);
    close(trace);

    // A partial dump would pass for a complete one, so failures remove it
    if (err != 0) {
        fprintf(stderr, "ktrace: cannot dump to %s: %s\n", path, strerror(err));
        unlink(path);
        return 1;
    }

    printf("wrote %llu bytes to %s\n", total, path);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc == 2 && strcmp(argv[1], "start") == 0) {
        return start_session();
    }

    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        return stop_session();
    }

    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        return show_status();
    }

    if (argc == 4 && strcmp(argv[1], "dump") == 0 && strcmp(argv[2], "-o") == 0) {
        return dump_trace(argv[3]);
    }

    fprintf(stderr, "usage: ktrace start | stop | status | dump -o FILE\n");
    return 1;
}
