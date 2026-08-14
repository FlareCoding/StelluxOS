/*
 * stlxtop - interactive system and process monitor
 *
 * Renders per-CPU utilization bars, memory usage, and a process table
 * sorted by CPU usage, all sampled from the /dev/sysinfo text files.
 * Refreshes once per second by default, tunable with -u <seconds>,
 * until q or Ctrl+C exits.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_CPUS      32
#define MAX_TASKS     256
#define NAME_MAX_LEN  64
#define STATE_MAX_LEN 12
#define FRAME_MAX     32768

typedef struct {
    uint64_t busy;
    uint64_t idle;
} cpu_sample_t;

typedef struct {
    uint32_t tid;
    uint32_t pid;
    uint32_t cpu;
    uint64_t ticks;
    char     state[STATE_MAX_LEN];
    char     name[NAME_MAX_LEN];
} task_sample_t;

typedef struct {
    uint64_t      tick_hz;
    uint32_t      cpu_count;
    cpu_sample_t  cpus[MAX_CPUS];
    uint64_t      mem_page_size;
    uint64_t      mem_total_pages;
    uint64_t      mem_used_pages;
    uint64_t      uptime_ns;
    int           task_count;
    task_sample_t tasks[MAX_TASKS];
} sample_t;

typedef struct {
    uint32_t tid;
    uint64_t ticks;
    unsigned pct;
} task_delta_t;

/* Input is polled without blocking and sleep slices pace the refresh,
 * so the loop stays responsive without trusting poll timeouts */
#define INPUT_SLICE_NS 50000000ULL

static uint64_t g_interval_ns = 1000000000ULL;

static sample_t g_cur;
static sample_t g_prev;
static int g_have_prev = 0;

static char g_frame[FRAME_MAX];
static size_t g_frame_len = 0;

static struct termios g_saved_tio;
static int g_tio_saved = 0;

/* ---- stats file reading ---- */

static ssize_t read_stats_file(const char* path, char* buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t rd = read(fd, buf + total, cap - 1 - total);
        if (rd <= 0) {
            break;
        }
        total += (size_t)rd;
    }
    close(fd);
    buf[total] = '\0';
    return (ssize_t)total;
}

static uint64_t labeled_field(const char* text, const char* label) {
    const char* p = strstr(text, label);
    if (!p) {
        return 0;
    }
    return strtoull(p + strlen(label), NULL, 10);
}

static int parse_cpu(sample_t* s) {
    char buf[2048];
    if (read_stats_file("/dev/sysinfo/cpu", buf, sizeof(buf)) <= 0) {
        return -1;
    }
    s->tick_hz = labeled_field(buf, "tick_hz ");
    s->cpu_count = 0;

    const char* p = buf;
    while (*p && s->cpu_count < MAX_CPUS) {
        if (strncmp(p, "cpu", 3) == 0) {
            char* end = NULL;
            strtoull(p + 3, &end, 10);
            cpu_sample_t* c = &s->cpus[s->cpu_count++];
            c->busy = strtoull(end, &end, 10);
            c->idle = strtoull(end, &end, 10);
        }
        while (*p && *p != '\n') {
            p++;
        }
        if (*p) {
            p++;
        }
    }
    return s->cpu_count > 0 ? 0 : -1;
}

static int parse_mem(sample_t* s) {
    char buf[512];
    if (read_stats_file("/dev/sysinfo/mem", buf, sizeof(buf)) <= 0) {
        return -1;
    }
    s->mem_page_size = labeled_field(buf, "page_size ");
    s->mem_total_pages = labeled_field(buf, "total_pages ");
    s->mem_used_pages = labeled_field(buf, "used_pages ");
    return 0;
}

static void parse_uptime(sample_t* s) {
    char buf[64];
    s->uptime_ns = 0;
    if (read_stats_file("/dev/sysinfo/uptime", buf, sizeof(buf)) > 0) {
        s->uptime_ns = strtoull(buf, NULL, 10);
    }
}

static int parse_tasks(sample_t* s) {
    static char buf[16384];
    if (read_stats_file("/dev/sysinfo/tasks", buf, sizeof(buf)) <= 0) {
        return -1;
    }

    s->task_count = 0;
    const char* p = buf;
    while (*p && s->task_count < MAX_TASKS) {
        task_sample_t* t = &s->tasks[s->task_count];
        char* end = NULL;

        t->tid = (uint32_t)strtoul(p, &end, 10);
        if (end == p) {
            break;
        }
        t->pid = (uint32_t)strtoul(end, &end, 10);

        while (*end == ' ') {
            end++;
        }
        size_t i = 0;
        while (*end && *end != ' ' && i < sizeof(t->state) - 1) {
            t->state[i++] = *end++;
        }
        t->state[i] = '\0';

        t->cpu = (uint32_t)strtoul(end, &end, 10);
        t->ticks = strtoull(end, &end, 10);

        while (*end == ' ') {
            end++;
        }
        i = 0;
        while (*end && *end != '\n' && i < sizeof(t->name) - 1) {
            t->name[i++] = *end++;
        }
        t->name[i] = '\0';

        s->task_count++;
        p = end;
        while (*p && *p != '\n') {
            p++;
        }
        if (*p) {
            p++;
        }
    }
    return 0;
}

static int sample_all(sample_t* s) {
    if (parse_cpu(s) != 0 || parse_mem(s) != 0 || parse_tasks(s) != 0) {
        return -1;
    }
    parse_uptime(s);
    return 0;
}

/* ---- terminal handling ---- */

static void restore_terminal(void) {
    if (g_tio_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tio);
    }
    const char* out = "\033[0m\033[?25h\033[2J\033[H";
    write(STDOUT_FILENO, out, strlen(out));
}

static void on_fatal_signal(int sig) {
    (void)sig;
    restore_terminal();
    _exit(0);
}

static void setup_terminal(void) {
    if (tcgetattr(STDIN_FILENO, &g_saved_tio) == 0) {
        g_tio_saved = 1;
        struct termios raw = g_saved_tio;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_fatal_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    const char* out = "\033[2J\033[?25l";
    write(STDOUT_FILENO, out, strlen(out));
}

static void query_winsize(int* rows, int* cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 25;
        *cols = 80;
    }
}

/* ---- frame building ---- */

static void emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t left = sizeof(g_frame) - g_frame_len;
    int n = vsnprintf(g_frame + g_frame_len, left, fmt, ap);
    va_end(ap);
    if (n > 0) {
        g_frame_len += ((size_t)n < left) ? (size_t)n : left - 1;
    }
}

static const char* pct_color(unsigned pct) {
    if (pct >= 80) {
        return "\033[31m";
    }
    if (pct >= 50) {
        return "\033[33m";
    }
    return "\033[32m";
}

static void emit_bar(const char* label, unsigned pct, const char* text,
                     int width) {
    int fill = (int)(pct * (unsigned)width / 100);
    if (fill > width) {
        fill = width;
    }
    emit("\033[1m%s\033[0m[%s", label, pct_color(pct));
    for (int i = 0; i < width; i++) {
        emit(i < fill ? "|" : " ");
    }
    emit("\033[0m] %s\033[K\r\n", text);
}

static unsigned cpu_pct(const cpu_sample_t* cur, const cpu_sample_t* prev) {
    uint64_t d_busy = cur->busy - prev->busy;
    uint64_t d_total = d_busy + (cur->idle - prev->idle);
    if (d_total == 0) {
        return 0;
    }
    return (unsigned)((d_busy * 100 + d_total / 2) / d_total);
}

static int delta_compare(const void* a, const void* b) {
    const task_delta_t* ta = (const task_delta_t*)a;
    const task_delta_t* tb = (const task_delta_t*)b;
    if (ta->pct != tb->pct) {
        return ta->pct > tb->pct ? -1 : 1;
    }
    if (ta->ticks != tb->ticks) {
        return ta->ticks > tb->ticks ? -1 : 1;
    }
    return ta->tid < tb->tid ? -1 : 1;
}

static uint64_t prev_task_ticks(uint32_t tid, int* found) {
    for (int i = 0; i < g_prev.task_count; i++) {
        if (g_prev.tasks[i].tid == tid) {
            *found = 1;
            return g_prev.tasks[i].ticks;
        }
    }
    *found = 0;
    return 0;
}

static const task_sample_t* task_by_tid(const sample_t* s, uint32_t tid) {
    for (int i = 0; i < s->task_count; i++) {
        if (s->tasks[i].tid == tid) {
            return &s->tasks[i];
        }
    }
    return NULL;
}

static void render(void) {
    int rows = 0;
    int cols = 0;
    query_winsize(&rows, &cols);

    g_frame_len = 0;
    emit("\033[H");

    /* Ticks one CPU accrued over the sample interval, the base for
     * per-task and per-CPU percentages */
    uint64_t interval_ticks = 0;
    if (g_have_prev) {
        for (uint32_t i = 0; i < g_cur.cpu_count; i++) {
            interval_ticks += (g_cur.cpus[i].busy - g_prev.cpus[i].busy)
                            + (g_cur.cpus[i].idle - g_prev.cpus[i].idle);
        }
        interval_ticks /= g_cur.cpu_count;
    }

    uint64_t up_s = g_cur.uptime_ns / 1000000000ULL;
    emit("\033[1mstlxtop\033[0m - up %llu:%02llu:%02llu, %d tasks\033[K\r\n",
         (unsigned long long)(up_s / 3600),
         (unsigned long long)((up_s / 60) % 60),
         (unsigned long long)(up_s % 60), g_cur.task_count);
    emit("\033[K\r\n");

    /* Per-CPU utilization bars */
    int bar_width = cols - 24;
    if (bar_width > 50) {
        bar_width = 50;
    }
    if (bar_width < 10) {
        bar_width = 10;
    }
    for (uint32_t i = 0; i < g_cur.cpu_count; i++) {
        unsigned pct = g_have_prev
                     ? cpu_pct(&g_cur.cpus[i], &g_prev.cpus[i]) : 0;
        char label[8];
        char text[16];
        snprintf(label, sizeof(label), "%3u", i);
        snprintf(text, sizeof(text), "%3u%%", pct);
        emit_bar(label, pct, text, bar_width);
    }

    /* Memory bar */
    uint64_t used_mb = g_cur.mem_used_pages * g_cur.mem_page_size
                     / (1024 * 1024);
    uint64_t total_mb = g_cur.mem_total_pages * g_cur.mem_page_size
                      / (1024 * 1024);
    unsigned mem_pct = 0;
    if (g_cur.mem_total_pages > 0) {
        mem_pct = (unsigned)(g_cur.mem_used_pages * 100
                             / g_cur.mem_total_pages);
    }
    char mem_text[40];
    snprintf(mem_text, sizeof(mem_text), "%llu/%lluMB",
             (unsigned long long)used_mb, (unsigned long long)total_mb);
    emit_bar("Mem", mem_pct, mem_text, bar_width);
    emit("\033[K\r\n");

    /* Task table sorted by CPU usage */
    emit("\033[7m%5s %5s %-8s %4s %5s %9s NAME\033[K\033[0m\r\n",
         "TID", "PID", "STATE", "CPU", "CPU%", "TIME");

    task_delta_t deltas[MAX_TASKS];
    int delta_count = 0;
    for (int i = 0; i < g_cur.task_count; i++) {
        const task_sample_t* t = &g_cur.tasks[i];
        task_delta_t* d = &deltas[delta_count++];
        d->tid = t->tid;
        d->ticks = t->ticks;
        d->pct = 0;
        if (g_have_prev && interval_ticks > 0) {
            int found = 0;
            uint64_t prev_ticks = prev_task_ticks(t->tid, &found);
            if (found && t->ticks >= prev_ticks) {
                uint64_t d_ticks = t->ticks - prev_ticks;
                d->pct = (unsigned)((d_ticks * 100 + interval_ticks / 2)
                                    / interval_ticks);
            }
        }
    }
    qsort(deltas, (size_t)delta_count, sizeof(deltas[0]), delta_compare);

    int table_rows = rows - 6 - (int)g_cur.cpu_count;
    if (table_rows < 1) {
        table_rows = 1;
    }
    for (int i = 0; i < delta_count && i < table_rows; i++) {
        const task_sample_t* t = task_by_tid(&g_cur, deltas[i].tid);
        if (!t) {
            continue;
        }
        uint64_t secs = g_cur.tick_hz > 0 ? t->ticks / g_cur.tick_hz : 0;
        char time_str[16];
        snprintf(time_str, sizeof(time_str), "%llu:%02llu.%02llu",
                 (unsigned long long)(secs / 60),
                 (unsigned long long)(secs % 60),
                 g_cur.tick_hz > 0
                     ? (unsigned long long)(t->ticks % g_cur.tick_hz)
                     : 0ULL);

        int name_width = cols - 42;
        if (name_width < 8) {
            name_width = 8;
        }
        emit("%5u %5u %-8s %4u %s%4u%%\033[0m %9s %.*s\033[K\r\n",
             t->tid, t->pid, t->state, t->cpu,
             pct_color(deltas[i].pct), deltas[i].pct,
             time_str, name_width, t->name);
    }

    emit("\033[0J\033[1mq\033[0m quit");

    // The terminal drains the pty at its own pace and short writes are
    // part of its contract, so push until the whole frame is delivered.
    // On error or no progress the frame is abandoned, the next refresh
    // repaints everything anyway
    size_t sent = 0;
    while (sent < g_frame_len) {
        ssize_t wr = write(STDOUT_FILENO, g_frame + sent, g_frame_len - sent);
        if (wr <= 0) {
            break;
        }
        sent += (size_t)wr;
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            double secs = strtod(argv[++i], NULL);
            if (secs < 0.1 || secs > 3600.0) {
                fprintf(stderr,
                        "stlxtop: -u expects seconds in 0.1..3600\n");
                return 1;
            }
            g_interval_ns = (uint64_t)(secs * 1000000000.0);
        }
    }

    if (sample_all(&g_cur) != 0) {
        fprintf(stderr, "stlxtop: cannot read /dev/sysinfo\n");
        return 1;
    }

    setup_terminal();

    int quit = 0;
    while (!quit) {
        render();

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL
                        + (uint64_t)now.tv_nsec;
        uint64_t deadline = now_ns + g_interval_ns;

        while (now_ns < deadline) {
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                char ch = 0;
                if (read(STDIN_FILENO, &ch, 1) == 1 &&
                    (ch == 'q' || ch == 'Q')) {
                    quit = 1;
                    break;
                }
            }

            uint64_t left = deadline - now_ns;
            uint64_t slice = left < INPUT_SLICE_NS ? left : INPUT_SLICE_NS;
            struct timespec rem = { 0, (long)slice };
            nanosleep(&rem, NULL);

            clock_gettime(CLOCK_MONOTONIC, &now);
            now_ns = (uint64_t)now.tv_sec * 1000000000ULL
                   + (uint64_t)now.tv_nsec;
        }
        if (quit) {
            break;
        }

        g_prev = g_cur;
        g_have_prev = 1;
        if (sample_all(&g_cur) != 0) {
            break;
        }
    }

    restore_terminal();
    return 0;
}
