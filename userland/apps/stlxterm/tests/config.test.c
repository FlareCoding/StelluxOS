/* The terminal's settings file: defaults when it is missing or silent, the clipboard keys it reads, and the
 * lines it leaves alone */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_REPORTS 8

static uint64_t g_checks;
static uint64_t g_failures;

static void check(int holds, const char *what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static term_config_t parsed(const char *text) {
    term_config_t config;
    config_defaults(&config);
    config_parse(&config, text, strlen(text));
    return config;
}

static void check_defaults(void) {
    term_config_t config;
    check(config_load(&config, "/nonexistent/stlxterm.conf") == -1, "a missing file reports it");
    check(!config.copy_on_select, "copy on select starts off");
    check(config.paste_protection, "paste protection starts on");
}

static void check_keys(void) {
    term_config_t config = parsed("[clipboard]\ncopy_on_select = true\npaste_protection = false\n");
    check(config.copy_on_select && !config.paste_protection, "the clipboard keys set both settings");

    config = parsed("[clipboard]\r\n  copy_on_select=true  \r\n");
    check(config.copy_on_select, "blanks and carriage returns around keys and values are ignored");

    config = parsed("[shell]\ncopy_on_select = true\n");
    check(!config.copy_on_select, "a key in another section is ignored");

    config = parsed("[clipboard]\ncopy_on_select = yes\n# copy_on_select = true\n; paste_protection = false\n");
    check(!config.copy_on_select && config.paste_protection, "other values and commented lines are ignored");

    config = parsed("[clipboard]\ncopy_on_select = true");
    check(config.copy_on_select, "a last line without a line break is read");
}

int main(void) {
    check_defaults();
    check_keys();

    if (g_failures != 0) {
        printf("config: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("config: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
