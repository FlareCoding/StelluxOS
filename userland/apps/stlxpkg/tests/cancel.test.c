/* cancel.c on the build machine, a process per case: SIGTERM, SIGINT and SIGPIPE fail the run as cancelled by the
 * first of them instead of ending it, end a blocked read at once, and turn a network failure into the cancel */
#define _POSIX_C_SOURCE 200809L

#include "cancel.h"
#include "report.h"

#include <stlxhttp/http.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_REPORTS 8
#define PATH_SIZE   256
#define TEXT_MAX    256
#define MS          1000000L

static uint64_t g_checks;
static uint64_t g_failures;
static char     g_err_path[PATH_SIZE];

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

static void pause_ms(long ms) {
    struct timespec pause = { ms / 1000, (ms % 1000) * MS };
    nanosleep(&pause, NULL);
}

static void raise_sigterm(void) {
    raise(SIGTERM);
    cancel_check();
}

static void raise_sigint(void) {
    raise(SIGINT);
    cancel_check();
}

static void raise_sigint_then_sigterm(void) {
    raise(SIGINT);
    raise(SIGTERM);
    cancel_check();
}

static void write_to_closed_pipe(void) {
    int ends[2];
    if (pipe(ends) != 0) {
        return;
    }

    close(ends[0]);
    if (write(ends[1], "x", 1) < 0 && errno == EPIPE) {
        cancel_check();
    }

    close(ends[1]);
}

/* A sender signals after 100 ms and writes after 2 s, so a read the signal did not end returns a byte */
static void interrupt_blocked_read(void) {
    int ends[2];
    if (pipe(ends) != 0) {
        return;
    }

    pid_t reader = getpid();
    pid_t sender = fork();
    if (sender == 0) {
        pause_ms(100);
        kill(reader, SIGTERM);
        pause_ms(2000);
        _exit(write(ends[1], "x", 1) == 1 ? 0 : 1);
    }

    char byte;
    if (read(ends[0], &byte, 1) < 0 && errno == EINTR) {
        cancel_check();
    }

    kill(sender, SIGKILL);
    waitpid(sender, NULL, 0);
}

static void fail_network_meanwhile(void) {
    raise(SIGTERM);
    fail_http(STLXHTTP_ERR_IO, "reading from github.com failed (errno 4)");
}

static void fail_without_cancel(void) {
    if (cancel_check() == 0) {
        fail(FAILURE_IO, "no cancel");
    }
}

/* Whether `scenario`, run in a process watching for cancels, exits by itself with the failure line `expected` */
static bool reports(void (*scenario)(void), const char* expected) {
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        char* text = NULL;
        size_t size = 0;
        FILE* events = open_memstream(&text, &size);
        report_use_events(events);
        cancel_watch();

        scenario();
        report_failed("sheen");
        fclose(events);
        _exit(strcmp(text, expected) == 0 ? 0 : 1);
    }

    int status = 0;
    waitpid(child, &status, 0);

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Whether a person sees only the cancel when SIGINT ends a wait for the network */
static bool says_cancelled(void) {
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        int err = open(g_err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(err, STDERR_FILENO);
        close(err);
        cancel_watch();

        raise(SIGINT);
        fail_http(STLXHTTP_ERR_IO, "reading from github.com failed (errno 4)");
        _exit(0);
    }

    int status = 0;
    waitpid(child, &status, 0);

    char text[TEXT_MAX];
    FILE* file = fopen(g_err_path, "r");
    size_t length = file ? fread(text, 1, sizeof(text) - 1, file) : 0;
    text[length] = '\0';
    if (file) {
        fclose(file);
    }

    return WIFEXITED(status) && strcmp(text, "stlxpkg: stopped by SIGINT\n") == 0;
}

int main(void) {
    snprintf(g_err_path, sizeof(g_err_path), "/tmp/stlxpkg-cancel-%d.err", (int)getpid());

    check(reports(raise_sigterm, "failed sheen cancelled stopped by SIGTERM\n"), "SIGTERM cancels");
    check(reports(raise_sigint, "failed sheen cancelled stopped by SIGINT\n"), "SIGINT cancels");
    check(reports(write_to_closed_pipe, "failed sheen cancelled stopped by SIGPIPE\n"),
          "writing to a pipe nobody reads cancels by SIGPIPE");
    check(reports(raise_sigint_then_sigterm, "failed sheen cancelled stopped by SIGINT\n"),
          "the first signal is the one reported");

    check(reports(interrupt_blocked_read, "failed sheen cancelled stopped by SIGTERM\n"),
          "a signal ends a blocked read at once");
    check(reports(fail_network_meanwhile, "failed sheen cancelled stopped by SIGTERM\n"),
          "a network failure during a cancel is reported as the cancel");
    check(reports(fail_without_cancel, "failed sheen io no cancel\n"), "without a signal nothing is cancelled");
    check(says_cancelled(), "without --events a person reads the cancel instead of the network failure");

    unlink(g_err_path);

    if (g_failures != 0) {
        printf("cancel: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("cancel: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
