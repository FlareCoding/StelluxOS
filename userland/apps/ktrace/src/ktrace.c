#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stlx/proc.h>

#define CONTROL_PATH "/dev/ktrace/control"
#define STATUS_PATH  "/dev/ktrace/status"
#define TRACE_PATH   "/dev/ktrace/trace"
#define NULL_PATH    "/dev/null"
#define SELF_PATH    "/bin/ktrace"

#define TRACE_READ_BYTES (1024 * 1024)

#define NS_PER_SEC  1000000000ull
#define NS_PER_MSEC 1000000ull

#define STATUS_POLL_NS   (10 * NS_PER_MSEC)
#define START_TIMEOUT_NS (3 * NS_PER_SEC)
#define STOP_TIMEOUT_NS  (30 * NS_PER_SEC)

static char g_status[16384];
static char g_trace_buffer[TRACE_READ_BYTES];
static volatile sig_atomic_t g_interrupted;

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

// Parses a `cpu<N> <records> <lost>` status line
static bool parse_cpu_line(const char* line, unsigned long* cpu, unsigned long long* records,
                           unsigned long long* lost) {
    if (strncmp(line, "cpu", 3) != 0) {
        return false;
    }

    char* end = NULL;
    *cpu = strtoul(line + 3, &end, 10);
    *records = strtoull(end, &end, 10);
    *lost = strtoull(end, &end, 10);

    return true;
}

static unsigned long long sum_lost_records(void) {
    unsigned long long total = 0;

    for (const char* line = g_status; *line; line = next_line(line)) {
        unsigned long cpu;
        unsigned long long records;
        unsigned long long lost;
        if (parse_cpu_line(line, &cpu, &records, &lost)) {
            total += lost;
        }
    }

    return total;
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
    printf("\ncpu  records  lost\n");

    for (const char* line = g_status; *line; line = next_line(line)) {
        unsigned long cpu;
        unsigned long long records;
        unsigned long long lost;
        if (parse_cpu_line(line, &cpu, &records, &lost)) {
            printf("%-4lu %-8llu %llu\n", cpu, records, lost);
        }
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
    printf("streamed      %llu bytes\n", (unsigned long long)status_u64("bytes_streamed"));
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

static void report_start_error(int err) {
    if (err == EBUSY) {
        fprintf(stderr, "ktrace: cannot start: a session is already recording\n");
    } else if (err == EPIPE) {
        fprintf(stderr, "ktrace: cannot start: nothing is reading the trace\n");
    } else {
        fprintf(stderr, "ktrace: cannot start: %s\n", strerror(err));
    }
}

static bool session_is_recording(void) {
    return status_state_is("recording") && status_u64("trace_open") == 1;
}

static bool reader_is_closed(void) {
    return status_u64("trace_open") == 0;
}

// Rereads the status until `condition` holds, leaving the last read in g_status
static bool poll_status(bool (*condition)(void), uint64_t timeout_ns) {
    uint64_t deadline_ns = monotonic_ns() + timeout_ns;

    while (true) {
        if (read_status() == 0 && condition()) {
            return true;
        }

        if (monotonic_ns() >= deadline_ns) {
            return false;
        }

        struct timespec pause = { 0, STATUS_POLL_NS };
        nanosleep(&pause, NULL);
    }
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

static void on_interrupt(int signal_number) {
    (void)signal_number;
    g_interrupted = 1;
}

// Without SA_RESTART, Ctrl-C ends the blocked read so the session can be stopped
static bool install_interrupt_handler(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_interrupt;
    sigemptyset(&action.sa_mask);

    return sigaction(SIGINT, &action, NULL) == 0;
}

// Returns 0 once the stream ends, or the errno value that cut it short
static int drain_trace(int trace, int out, unsigned long long* written) {
    bool stop_sent = false;

    while (true) {
        ssize_t n = read(trace, g_trace_buffer, sizeof(g_trace_buffer));
        if (n == 0) {
            return 0;
        }

        if (n < 0 && errno == EINTR) {
            if (g_interrupted && !stop_sent) {
                // Fails harmlessly when `ktrace stop` ended the session first
                send_command("stop");
                stop_sent = true;
            }

            continue;
        }

        if (n < 0 || !write_all(out, g_trace_buffer, (size_t)n)) {
            return errno;
        }

        *written += (unsigned long long)n;
    }
}

static int record_session(const char* path) {
    if (!install_interrupt_handler()) {
        fprintf(stderr, "ktrace: cannot handle Ctrl-C: %s\n", strerror(errno));
        return 1;
    }

    int trace = open(TRACE_PATH, O_RDONLY);
    if (trace < 0) {
        report_start_error(errno);
        return 1;
    }

    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        fprintf(stderr, "ktrace: %s: %s\n", path, strerror(errno));
        close(trace);
        return 1;
    }

    int err = send_command("start");
    if (err != 0) {
        report_start_error(err);
        close(out);
        close(trace);
        return 1;
    }

    printf("recording to %s\n", path);

    unsigned long long written = 0;
    err = drain_trace(trace, out, &written);

    close(out);
    close(trace);

    if (err != 0) {
        fprintf(stderr, "ktrace: cannot record to %s: %s\n", path, strerror(err));
        return 1;
    }

    unsigned long long lost = read_status() == 0 ? sum_lost_records() : 0;
    printf("wrote %llu bytes to %s, %llu records lost\n", written, path, lost);

    return 0;
}

// The process must not have started yet
static int detach_standard_streams(int process) {
    int null_fd = open(NULL_PATH, O_RDWR);
    if (null_fd < 0) {
        return errno;
    }

    int err = 0;
    for (int slot = STDIN_FILENO; slot <= STDERR_FILENO && err == 0; slot++) {
        if (proc_set_handle(process, slot, null_fd) != 0) {
            err = errno;
        }
    }

    close(null_fd);
    return err;
}

// Runs `ktrace record -o path` on its own. It must hold none of this command's
// streams, or an SSH session that ran `ktrace start` cannot close.
static int spawn_detached_recorder(const char* path) {
    const char* argv[] = { "record", "-o", path, NULL };
    int recorder = proc_create(SELF_PATH, argv);
    if (recorder < 0) {
        return errno;
    }

    int err = detach_standard_streams(recorder);
    if (err == 0 && proc_start(recorder) != 0) {
        err = errno;
    }

    if (err != 0) {
        close(recorder);
        return err;
    }

    proc_detach(recorder);
    return 0;
}

static int start_detached_recording(const char* path) {
    if (read_status() != 0) {
        fprintf(stderr, "ktrace: %s: %s\n", STATUS_PATH, strerror(errno));
        return 1;
    }

    if (status_u64("trace_open") > 0) {
        report_start_error(EBUSY);
        return 1;
    }

    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        fprintf(stderr, "ktrace: %s: %s\n", path, strerror(errno));
        return 1;
    }

    close(out);

    int err = spawn_detached_recorder(path);
    if (err != 0) {
        fprintf(stderr, "ktrace: cannot start the recorder: %s\n", strerror(err));
        return 1;
    }

    if (!poll_status(session_is_recording, START_TIMEOUT_NS)) {
        fprintf(stderr, "ktrace: cannot start: the recorder did not start a session\n");
        return 1;
    }

    printf("recording to %s\n", path);
    return 0;
}

static int stop_session(void) {
    int err = send_command("stop");
    if (err == EINVAL) {
        fprintf(stderr, "ktrace: cannot stop: no session is recording\n");
        return 1;
    }

    if (err != 0) {
        fprintf(stderr, "ktrace: cannot stop: %s\n", strerror(err));
        return 1;
    }

    if (!poll_status(reader_is_closed, STOP_TIMEOUT_NS)) {
        fprintf(stderr, "ktrace: stopped, but the recorder has not finished the file\n");
        return 1;
    }

    printf("streamed %llu bytes, %llu records lost\n",
           (unsigned long long)status_u64("bytes_streamed"), sum_lost_records());

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc == 4 && strcmp(argv[1], "start") == 0 && strcmp(argv[2], "-o") == 0) {
        return start_detached_recording(argv[3]);
    }

    if (argc == 4 && strcmp(argv[1], "record") == 0 && strcmp(argv[2], "-o") == 0) {
        return record_session(argv[3]);
    }

    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        return stop_session();
    }

    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        return show_status();
    }

    fprintf(stderr, "usage: ktrace start -o FILE | record -o FILE | stop | status\n");
    return 1;
}
