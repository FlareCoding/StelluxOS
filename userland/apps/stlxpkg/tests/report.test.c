/* report.c on the build machine: with --events each fact is one exact line, progress at most every 100 ms and at its
 * length, and the first failure kept, and without it the same sentences as before and no facts */
#define _POSIX_C_SOURCE 200809L

#include "report.h"

#include <stlxhttp/http.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_REPORTS 8
#define PATH_SIZE   256
#define TEXT_MAX    4096
#define MS          1000000ull

static uint64_t g_checks;
static uint64_t g_failures;
static FILE*    g_events;
static char*    g_events_text;
static size_t   g_events_size;
static char     g_out_path[PATH_SIZE];
static char     g_err_path[PATH_SIZE];
static int      g_saved_out;
static int      g_saved_err;

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

static void begin_events(void) {
    g_events = open_memstream(&g_events_text, &g_events_size);
    report_use_events(g_events);
}

/* Whether the lines printed since begin_events are exactly `expected` */
static bool end_events(const char* expected) {
    report_use_events(NULL);
    fclose(g_events);
    bool holds = strcmp(g_events_text, expected) == 0;
    free(g_events_text);

    return holds;
}

/* Points stdout and stderr at files until end_capture */
static void begin_capture(void) {
    fflush(stdout);
    fflush(stderr);
    g_saved_out = dup(STDOUT_FILENO);
    g_saved_err = dup(STDERR_FILENO);

    int out = open(g_out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int err = open(g_err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(out, STDOUT_FILENO);
    dup2(err, STDERR_FILENO);
    close(out);
    close(err);
}

static void end_capture(void) {
    fflush(stdout);
    fflush(stderr);
    dup2(g_saved_out, STDOUT_FILENO);
    dup2(g_saved_err, STDERR_FILENO);
    close(g_saved_out);
    close(g_saved_err);
}

static bool file_holds(const char* path, const char* expected) {
    char text[TEXT_MAX];
    FILE* file = fopen(path, "r");
    size_t length = file ? fread(text, 1, sizeof(text) - 1, file) : 0;
    text[length] = '\0';
    if (file) {
        fclose(file);
    }

    return strcmp(text, expected) == 0;
}

static void check_install(void) {
    begin_events();
    report_start("sheen", "1.0-1", 7822994);
    check(g_events_size == strlen("start sheen 1.0-1 7822994\n"), "each line goes out as it happens");

    report_progress("sheen", 65536, 7822994, 1000 * MS);
    report_progress("sheen", 131072, 7822994, 1099 * MS);
    report_progress("sheen", 196608, 7822994, 1100 * MS);
    report_progress("sheen", 262144, 7822994, 1150 * MS);
    report_progress("sheen", 7822994, 7822994, 1160 * MS);

    report_commit("sheen");
    report_installed("sheen", "1.0-1", 24498796);
    check(end_events("start sheen 1.0-1 7822994\n"
                     "progress sheen 65536 7822994\n"
                     "progress sheen 196608 7822994\n"
                     "progress sheen 7822994 7822994\n"
                     "commit sheen\n"
                     "installed sheen 1.0-1 24498796\n"),
          "an install prints progress at most every 100 ms and at its length");

    begin_events();
    report_start("a", "1-1", 10);
    report_progress("a", 10, 10, 2000 * MS);

    report_start("b", "2-1", -1);
    report_progress("b", 5, -1, 2010 * MS);
    report_progress("b", 9, -1, 2100 * MS);
    report_progress("b", 12, -1, 2110 * MS);
    check(end_events("start a 1-1 10\n"
                     "progress a 10 10\n"
                     "start b 2-1 -1\n"
                     "progress b 5 -1\n"
                     "progress b 12 -1\n"),
          "each install's first progress goes out at once, and an unstated length is -1");
}

static void check_list_and_remove(void) {
    begin_events();
    report_record("python", "3.12.8-1");
    report_package("sheen", "1.0-1", "available");
    report_package("python", "3.12.8-2", "update");
    report_removed("python", "3.12.8-1");
    check(end_events("record python 3.12.8-1\n"
                     "package sheen 1.0-1 available\n"
                     "package python 3.12.8-2 update\n"
                     "removed python 3.12.8-1\n"),
          "records, packages and removals print one line each");
}

static void check_failures(void) {
    begin_events();
    check(fail_http(STLXHTTP_ERR_TIMEOUT, "github.com sent nothing for 30 s") == -1, "a failure returns -1");
    fail(FAILURE_IO, "cannot delete %s (errno %d)", "/var/lib/stlxpkg/staging", EBUSY);
    report_failed("python");

    fail(FAILURE_LOCK, "another stlxpkg is running");
    report_failed("-");

    fail(failure_of_errno(ENOMEM), "out of memory");
    report_failed("gcc");
    fail(failure_of_errno(EACCES), "cannot stage %s (errno %d)", "/usr/bin/gcc", EACCES);
    report_failed("gcc");

    fail_http(STLXHTTP_ERR_NO_MEMORY, "out of memory");
    report_failed("gcc");
    fail_http(STLXHTTP_ERR_TRUNCATED, "github.com ended the reply after 4 of 9 bytes");
    report_failed("gcc");

    check(end_events("failed python timeout github.com sent nothing for 30 s\n"
                     "failed - lock another stlxpkg is running\n"
                     "failed gcc memory out of memory\n"
                     "failed gcc io cannot stage /usr/bin/gcc (errno 13)\n"
                     "failed gcc memory out of memory\n"
                     "failed gcc network github.com ended the reply after 4 of 9 bytes\n"),
          "a failure line carries the run's first failure, its kind and its sentence");
}

static void check_terminal(void) {
    begin_capture();
    report_say("installed %s %s%s%s", "qt", "6.8.1-1", " under ", "/usr/sdk");
    fail(FAILURE_CONFLICT, "%s cannot install %s, which belongs to %s", "sheen", "/usr/bin/sheen", "vim");

    report_record("python", "3.12.8-1");
    report_package("python", "3.12.8-1", "installed");
    report_start("sheen", "1.0-1", 100);
    report_progress("sheen", 100, 100, 0);

    report_commit("sheen");
    report_installed("sheen", "1.0-1", 100);
    report_removed("sheen", "1.0-1");
    report_failed("sheen");
    end_capture();

    check(file_holds(g_out_path, "stlxpkg: installed qt 6.8.1-1 under /usr/sdk\n"),
          "without --events a person reads sentences on stdout and no facts");
    check(file_holds(g_err_path, "stlxpkg: sheen cannot install /usr/bin/sheen, which belongs to vim\n"),
          "without --events a failure is a sentence on stderr");

    begin_capture();
    begin_events();
    report_say("downloading %s", "sheen");
    fail(FAILURE_NETWORK, "cannot resolve %s", "github.com");
    report_failed("sheen");

    bool lines = end_events("failed sheen network cannot resolve github.com\n");
    end_capture();
    check(lines && file_holds(g_out_path, "") && file_holds(g_err_path, ""),
          "with --events no sentence is printed");
}

int main(void) {
    snprintf(g_out_path, sizeof(g_out_path), "/tmp/stlxpkg-report-%d.out", (int)getpid());
    snprintf(g_err_path, sizeof(g_err_path), "/tmp/stlxpkg-report-%d.err", (int)getpid());

    check_install();
    check_list_and_remove();
    check_failures();
    check_terminal();

    unlink(g_out_path);
    unlink(g_err_path);

    if (g_failures != 0) {
        printf("report: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("report: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
