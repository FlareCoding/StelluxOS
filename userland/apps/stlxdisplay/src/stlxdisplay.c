#define _POSIX_C_SOURCE 200809L
#include <stlxwin/stlxwin.h>

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MILLIHERTZ_PER_HERTZ 1000u

/* A typed refresh rate may differ from the listed one by its rounding */
#define REFRESH_TOLERANCE_MILLIHERTZ 500u

static const char USAGE[] =
    "usage: stlxdisplay                                   list each output's modes\n"
    "       stlxdisplay <output> <width>x<height>@<hertz> [--keep]\n"
    "                                                     light the output at a listed mode,\n"
    "                                                     as a 15 second trial unless --keep\n";

static const char* const OUTCOME_TEXT[] = {
    [STLXWIN_MODE_SET]        = "set",
    [STLXWIN_MODE_TRIAL]      = "on trial",
    [STLXWIN_MODE_KEPT]       = "kept",
    [STLXWIN_MODE_SET_BACK]   = "set back",
    [STLXWIN_MODE_DEFAULT]    = "set to its default after a failed set-back",
    [STLXWIN_MODE_NO_BUFFERS] = "set back, the desktop has no buffers at that size",
    [STLXWIN_MODE_BUSY]       = "refused, a trial is running",
    [STLXWIN_MODE_NO_TRIAL]   = "refused, the trial had already ended",
    [STLXWIN_MODE_INVALID]    = "refused as invalid",
};

/* What did not happen when the display refused the mode asked for last */
static const char* const FAILED_TEXT[] = {
    [STLXWIN_MODE_SET]        = "not set",
    [STLXWIN_MODE_TRIAL]      = "not set, so no trial started",
    [STLXWIN_MODE_SET_BACK]   = "not set back",
    [STLXWIN_MODE_DEFAULT]    = "not set back, nor set to its default",
    [STLXWIN_MODE_NO_BUFFERS] = "the desktop has no buffers at that size, and setting it back failed",
};

static bool modes_equal(const stlxwin_mode* a, const stlxwin_mode* b) {
    return a->width == b->width && a->height == b->height && a->refresh_millihertz == b->refresh_millihertz;
}

static void print_mode(const stlxwin_mode* mode) {
    printf("%ux%u@%llu.%03llu", mode->width, mode->height,
           (unsigned long long)(mode->refresh_millihertz / MILLIHERTZ_PER_HERTZ),
           (unsigned long long)(mode->refresh_millihertz % MILLIHERTZ_PER_HERTZ));
}

/* Reads "<width>x<height>@<hertz>", the hertz with up to three decimals. Returns 0, or -1 when malformed */
static int parse_mode(const char* text, stlxwin_mode* out) {
    char* end;
    unsigned long width = strtoul(text, &end, 10);
    if (end == text || *end != 'x') {
        return -1;
    }

    const char* height_text = end + 1;
    unsigned long height = strtoul(height_text, &end, 10);
    if (end == height_text || *end != '@') {
        return -1;
    }

    const char* hertz_text = end + 1;
    unsigned long hertz = strtoul(hertz_text, &end, 10);
    if (end == hertz_text) {
        return -1;
    }

    uint64_t millihertz = (uint64_t)hertz * MILLIHERTZ_PER_HERTZ;
    if (*end == '.') {
        end++;
        for (uint64_t scale = MILLIHERTZ_PER_HERTZ / 10; scale > 0 && isdigit((unsigned char)*end); scale /= 10) {
            millihertz += (uint64_t)(*end - '0') * scale;
            end++;
        }
    }

    bool is_sized = width > 0 && height > 0 && width <= UINT32_MAX && height <= UINT32_MAX;
    if (*end != '\0' || !is_sized) {
        return -1;
    }

    out->width = (uint32_t)width;
    out->height = (uint32_t)height;
    out->refresh_millihertz = millihertz;

    return 0;
}

/* The listed mode of the asked size whose refresh rate is nearest the asked one within the tolerance, the earlier on a
 * tie, or NULL */
static const stlxwin_mode* find_mode(const stlxwin_mode* modes, long count, const stlxwin_mode* asked) {
    const stlxwin_mode* best = NULL;
    uint64_t best_distance = 0;

    for (long i = 0; i < count; i++) {
        const stlxwin_mode* mode = &modes[i];
        if (mode->width != asked->width || mode->height != asked->height) {
            continue;
        }

        uint64_t distance = mode->refresh_millihertz > asked->refresh_millihertz
                          ? mode->refresh_millihertz - asked->refresh_millihertz
                          : asked->refresh_millihertz - mode->refresh_millihertz;
        bool is_nearer = !best || distance < best_distance;
        if (distance <= REFRESH_TOLERANCE_MILLIHERTZ && is_nearer) {
            best = mode;
            best_distance = distance;
        }
    }

    return best;
}

/* Waits for the output's MODES, dropping other events. Returns 0, or -1 once the display manager is gone */
static int wait_modes(stlxwin_conn* conn, uint32_t output) {
    stlxwin_event ev;
    while (1) {
        while (stlxwin_next_event(conn, &ev)) {
            if (ev.type == STLXWIN_EVT_MODES && ev.modes.output == output) {
                return 0;
            }
        }

        if (stlxwin_wait(conn, -1) < 0) {
            return -1;
        }
    }
}

/* Waits for the output's next mode result, dropping other events. Returns 0, or -1 once the display manager is gone */
static int wait_result(stlxwin_conn* conn, uint32_t output, stlxwin_event* ev) {
    while (1) {
        while (stlxwin_next_event(conn, ev)) {
            if (ev->type == STLXWIN_EVT_MODE_RESULT && ev->mode_result.output == output) {
                return 0;
            }
        }

        if (stlxwin_wait(conn, -1) < 0) {
            return -1;
        }
    }
}

/* Asks for an output's modes and waits for them. Returns the count, or -1 when the display manager is gone or the modes
 * cannot be kept */
static long fetch_modes(stlxwin_conn* conn, uint32_t output, stlxwin_modes_info* info, stlxwin_mode* modes) {
    if (stlxwin_modes_watch(conn, output) < 0 || wait_modes(conn, output) < 0) {
        return -1;
    }

    return stlxwin_modes_get(conn, output, info, modes, STLXWIN_MODES_MAX);
}

static void print_output(uint32_t output, const stlxwin_modes_info* info, const stlxwin_mode* modes, long count) {
    bool is_lit = (info->flags & STLXWIN_OUTPUT_LIT) != 0;
    bool is_primary = (info->flags & STLXWIN_OUTPUT_PRIMARY) != 0;
    printf("output %u: %s%s\n", output, is_lit ? "lit" : "dark", is_primary ? ", primary" : "");

    for (long i = 0; i < count; i++) {
        bool is_current = is_lit && modes_equal(&modes[i], &info->current);
        printf("  ");
        print_mode(&modes[i]);
        printf("%s%s\n", i == 0 ? "  default" : "", is_current ? "  current" : "");
    }
}

/* Prints a mode result. Returns whether the display manager did what was asked without an error */
static bool print_result(const stlxwin_event* ev) {
    uint32_t outcome = ev->mode_result.outcome;
    bool is_failed = ev->mode_result.result != 0;
    const char* const* texts = is_failed ? FAILED_TEXT : OUTCOME_TEXT;
    size_t text_count = is_failed ? sizeof(FAILED_TEXT) / sizeof(FAILED_TEXT[0])
                                  : sizeof(OUTCOME_TEXT) / sizeof(OUTCOME_TEXT[0]);
    bool is_named = outcome < text_count && texts[outcome];

    printf("output %u: %s", ev->mode_result.output, is_named ? texts[outcome] : "answered");
    if (is_failed) {
        printf(", the display answered: %s", strerror(-ev->mode_result.result));
    }

    if (ev->mode_result.mode.width != 0) {
        printf(", lit at ");
        print_mode(&ev->mode_result.mode);
    } else {
        printf(", dark");
    }

    printf("\n");

    bool is_done = outcome == STLXWIN_MODE_SET || outcome == STLXWIN_MODE_TRIAL || outcome == STLXWIN_MODE_KEPT
                || outcome == STLXWIN_MODE_SET_BACK;
    return is_done && ev->mode_result.result == 0;
}

static int list_outputs(stlxwin_conn* conn) {
    uint32_t count = 1;
    for (uint32_t output = 0; output < count; output++) {
        stlxwin_modes_info info;
        stlxwin_mode modes[STLXWIN_MODES_MAX];
        long listed = fetch_modes(conn, output, &info, modes);
        if (listed < 0) {
            fprintf(stderr, "stlxdisplay: the display manager went away\n");
            return 1;
        }

        count = info.output_count < STLXWIN_OUTPUTS_MAX ? info.output_count : STLXWIN_OUTPUTS_MAX;
        if (output < count) {
            print_output(output, &info, modes, listed);
        }
    }

    if (count == 0) {
        printf("no outputs\n");
    }

    return 0;
}

/* Waits for the user's answer or the trial's own set-back, whichever comes first. Returns the exit status */
static int ask_keep(stlxwin_conn* conn, uint32_t output) {
    printf("Keep this mode? Setting it back in 15 seconds [y/N] ");
    fflush(stdout);

    struct pollfd fds[2] = {
        { STDIN_FILENO, POLLIN, 0 },
        { stlxwin_conn_fd(conn), POLLIN, 0 },
    };

    stlxwin_event ev;
    while (1) {
        while (stlxwin_next_event(conn, &ev)) {
            if (ev.type == STLXWIN_EVT_MODE_RESULT && ev.mode_result.output == output) {
                printf("\n");
                return print_result(&ev) ? 0 : 1;
            }
        }

        int rc = poll(fds, 2, -1);
        if (rc < 0 && errno != EINTR) {
            return 1;
        }

        if ((fds[0].revents & (POLLIN | POLLHUP)) != 0) {
            char answer[16];
            ssize_t n = read(STDIN_FILENO, answer, sizeof(answer));
            bool keep = n > 0 && (answer[0] == 'y' || answer[0] == 'Y');
            if (stlxwin_mode_keep(conn, output, keep) < 0 || wait_result(conn, output, &ev) < 0) {
                fprintf(stderr, "stlxdisplay: the display manager went away\n");
                return 1;
            }

            /* A y that reaches the display manager after the timer gets the set-back's answer first */
            bool is_done = print_result(&ev);
            bool is_kept = ev.mode_result.outcome == STLXWIN_MODE_KEPT;
            return is_done && (is_kept || !keep) ? 0 : 1;
        }

        if (fds[1].revents != 0 && stlxwin_dispatch(conn) < 0) {
            fprintf(stderr, "stlxdisplay: the display manager went away\n");
            return 1;
        }
    }
}

static int request_mode(stlxwin_conn* conn, const char* output_text, const char* mode_text, bool keep) {
    char* end;
    unsigned long output = strtoul(output_text, &end, 10);
    stlxwin_mode asked;
    bool is_output = end != output_text && *end == '\0' && output < STLXWIN_OUTPUTS_MAX;
    if (!is_output || parse_mode(mode_text, &asked) < 0) {
        fputs(USAGE, stderr);
        return 2;
    }

    stlxwin_modes_info info;
    stlxwin_mode modes[STLXWIN_MODES_MAX];
    long count = fetch_modes(conn, (uint32_t)output, &info, modes);
    if (count < 0) {
        fprintf(stderr, "stlxdisplay: the display manager went away\n");
        return 1;
    }

    const stlxwin_mode* mode = find_mode(modes, count, &asked);
    if (!mode) {
        fprintf(stderr, "stlxdisplay: output %lu lists no %s, run stlxdisplay with no arguments to list its modes\n",
                output, mode_text);
        return 1;
    }

    printf("asking output %lu for ", output);
    print_mode(mode);
    fputs(keep ? "\n" : " as a 15 second trial\n", stdout);

    stlxwin_event ev;
    uint32_t flags = keep ? 0 : STLXWIN_SET_MODE_TRIAL;
    if (stlxwin_mode_set(conn, (uint32_t)output, mode, flags) < 0 || wait_result(conn, (uint32_t)output, &ev) < 0) {
        fprintf(stderr, "stlxdisplay: the display manager went away\n");
        return 1;
    }

    bool is_done = print_result(&ev);
    bool is_trial = is_done && ev.mode_result.outcome == STLXWIN_MODE_TRIAL;
    if (!is_trial) {
        return is_done ? 0 : 1;
    }

    return ask_keep(conn, (uint32_t)output);
}

int main(int argc, char** argv) {
    bool is_listing = argc == 1;
    bool is_request = argc == 3 || (argc == 4 && strcmp(argv[3], "--keep") == 0);
    if (!is_listing && !is_request) {
        fputs(USAGE, stderr);
        return 2;
    }

    /* A write to a display manager that just went away fails with EPIPE instead of ending stlxdisplay unannounced */
    signal(SIGPIPE, SIG_IGN);

    stlxwin_conn* conn = stlxwin_connect("stlxdisplay");
    if (!conn) {
        fprintf(stderr, "stlxdisplay: no display manager to ask\n");
        return 1;
    }

    int status = is_listing ? list_outputs(conn) : request_mode(conn, argv[1], argv[2], argc == 4);
    stlxwin_disconnect(conn);

    return status;
}
