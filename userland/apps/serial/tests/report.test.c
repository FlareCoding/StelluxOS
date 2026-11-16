/* report.c on the build machine: with --events each fact is one exact tab-separated line, and without it a person
 * reads padded columns and sentences, with failures on stderr */
#define _POSIX_C_SOURCE 200809L

#include "report.h"

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

// Lines shaped as the kernel writes them, with made-up USB IDs
#define FTDI_LINE   "ft232bm-0001\tusb\tftdi\t1234:5678\t1.2\tFT0001\tAcme\tAcme UART\t3\t42\t0\t-"
#define BRIDGE_LINE \
    "cp2102-0007\tusb\tcp210x\t1234:9abc\t1.5.3\t0007\tSilicon Bridges\tUSB to UART Bridge\t7\t789\t0\t-"

static uint64_t g_checks;
static uint64_t g_failures;
static FILE*    g_out;
static char*    g_out_text;
static size_t   g_out_size;
static char     g_err_path[PATH_SIZE];
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

static void parse(const char* snapshot, listing* out) {
    size_t length = strlen(snapshot);
    char* text = malloc(length + 1);
    memcpy(text, snapshot, length);
    listing_parse(text, length, out);
}

static void begin_report(bool has_events) {
    g_out = open_memstream(&g_out_text, &g_out_size);
    report_use(g_out, has_events);
}

/* Whether the reports since begin_report are exactly `expected` */
static bool end_report(const char* expected) {
    fclose(g_out);
    bool holds = strcmp(g_out_text, expected) == 0;
    free(g_out_text);

    return holds;
}

/* Points stderr at a file until end_stderr_capture */
static void begin_stderr_capture(void) {
    fflush(stderr);
    g_saved_err = dup(STDERR_FILENO);

    int err = open(g_err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(err, STDERR_FILENO);
    close(err);
}

/* Whether stderr took exactly `expected` since begin_stderr_capture */
static bool end_stderr_capture(const char* expected) {
    fflush(stderr);
    dup2(g_saved_err, STDERR_FILENO);
    close(g_saved_err);

    char text[TEXT_MAX];
    FILE* file = fopen(g_err_path, "r");
    size_t length = file ? fread(text, 1, sizeof(text) - 1, file) : 0;
    text[length] = '\0';
    if (file) {
        fclose(file);
    }

    return strcmp(text, expected) == 0;
}

static void check_events(void) {
    listing ports;
    parse(FTDI_LINE "\n" BRIDGE_LINE "\n", &ports);

    begin_report(true);
    report_ports(&ports);
    check(end_report("port\t" FTDI_LINE "\nport\t" BRIDGE_LINE "\nlisted\t2\n"),
          "each port is a port line with the listing's whole line, then listed with their count");

    listing none = { 0 };
    begin_report(true);
    report_ports(&none);
    check(end_report("listed\t0\n"), "with no port, listed 0 says so");

    begin_report(true);
    report_removed(&ports.ports[1]);
    report_added(&ports.ports[1]);
    check(end_report("remove\tcp2102-0007\nadd\t" BRIDGE_LINE "\n"), "a departure names the port, an arrival lists it");

    begin_report(true);
    begin_stderr_capture();
    check(report_failure("-", FAILURE_IO, "cannot read %s", LISTING_PATH) == 1, "a failure returns exit status 1");
    report_failure("cp2102-0007", FAILURE_IO, "cannot read %s", "cp2102-0007");
    bool is_stderr_quiet = end_stderr_capture("");
    check(end_report("failed\t-\tio\tcannot read /dev/sysinfo/serial\n"
                     "failed\tcp2102-0007\tio\tcannot read cp2102-0007\n") &&
              is_stderr_quiet,
          "a failure line carries its subject, kind and reason, and no sentence goes to stderr");

    listing_free(&ports);
}

static void check_sentences(void) {
    listing ports;
    parse(BRIDGE_LINE "\n" FTDI_LINE "\n", &ports);

    begin_report(false);
    report_ports(&ports);
    check(end_report("cp2102-0007   USB to UART Bridge  Silicon Bridges  1234:9abc  0007    1.5.3\n"
                     "ft232bm-0001  Acme UART           Acme             1234:5678  FT0001  1.2\n"),
          "each column is padded to its widest value, and the last is not padded");

    listing none = { 0 };
    begin_report(false);
    report_ports(&none);
    check(end_report("No serial ports.\n"), "with no port, a sentence says so");

    begin_report(false);
    report_removed(&ports.ports[0]);
    report_added(&ports.ports[0]);
    check(end_report("removed  cp2102-0007\n"
                     "added    cp2102-0007  USB to UART Bridge  Silicon Bridges  1234:9abc  0007  1.5.3\n"),
          "a departure and an arrival read as lines that line up");

    begin_report(false);
    begin_stderr_capture();
    report_failure("-", FAILURE_IO, "cannot read %s", LISTING_PATH);
    bool is_sentence = end_stderr_capture("serial: cannot read /dev/sysinfo/serial\n");
    check(end_report("") && is_sentence, "without --events a failure is a sentence on stderr");
    listing_free(&ports);
}

int main(void) {
    snprintf(g_err_path, sizeof(g_err_path), "/tmp/serial-report-%d.err", (int)getpid());

    check_events();
    check_sentences();

    unlink(g_err_path);

    if (g_failures != 0) {
        printf("report: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("report: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
