/* The capture bar's starting options, run on the build machine: the defaults, the main file's values kept against
 * drop-ins, values the bar does not offer left as they were, and a save writing them back */

#include <stlxconf/conf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_REPORTS 8

static uint64_t   g_checks;
static uint64_t   g_failures;
static stlxconf_t g_conf;
static stlxconf_t g_reloaded;
static stlxconf_t g_odd;
static char       g_dir[] = "/tmp/stlxconf-screenshot-XXXXXX";

static const char MAIN_TEXT[] =
    "[screenshot]\n"
    "timer=10\n"
    "shadow=off\n"
    "pointer=on\n"
    "remember_region=off\n";

static const char DROP_IN_TEXT[] =
    "[screenshot]\n"
    "timer=5\n"
    "pointer=off\n";

static const char ODD_TEXT[] =
    "[screenshot]\n"
    "timer=7\n"
    "shadow=maybe\n";

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

static void write_file(const char* path, const char* text) {
    FILE* file = fopen(path, "w");
    if (!file) {
        check(false, path);
        return;
    }

    fputs(text, file);
    fclose(file);
}

static bool holds_main_values(const stlxconf_t* conf) {
    return conf->screenshot_timer == 10 && !conf->screenshot_shadow && conf->screenshot_pointer &&
           !conf->screenshot_remember_region;
}

int main(void) {
    if (!mkdtemp(g_dir)) {
        printf("screenshot: cannot make a scratch directory\n");
        return 1;
    }

    char main_path[256];
    char saved_path[256];
    char odd_path[256];
    char drop_in_dir[256];
    char drop_in_path[512];

    snprintf(main_path, sizeof(main_path), "%s/stlxdm.conf", g_dir);
    snprintf(saved_path, sizeof(saved_path), "%s/saved.conf", g_dir);
    snprintf(odd_path, sizeof(odd_path), "%s/odd.conf", g_dir);
    snprintf(drop_in_dir, sizeof(drop_in_dir), "%s/conf.d", g_dir);
    snprintf(drop_in_path, sizeof(drop_in_path), "%s/greedy.conf", drop_in_dir);

    mkdir(drop_in_dir, 0755);
    write_file(main_path, MAIN_TEXT);
    write_file(drop_in_path, DROP_IN_TEXT);
    write_file(odd_path, ODD_TEXT);

    stlxconf_defaults(&g_conf);
    check(g_conf.screenshot_timer == 0 && g_conf.screenshot_shadow && !g_conf.screenshot_pointer &&
              g_conf.screenshot_remember_region,
          "the options start with no timer, the shadow kept, the pointer left out and the last region kept");

    check(stlxconf_load(&g_conf, main_path) == 0, "the main file loads");
    check(stlxconf_load_drop_ins(&g_conf, drop_in_dir) == 0, "the drop-in files load");
    check(holds_main_values(&g_conf), "the main file sets the options and a drop-in cannot");

    check(stlxconf_load(&g_odd, odd_path) == 0, "a file with values the bar does not offer loads");
    check(g_odd.screenshot_timer == 0 && g_odd.screenshot_shadow,
          "a timer the bar does not offer and a switch neither on nor off stay as they were");

    check(stlxconf_save(&g_conf, saved_path) == 0, "the configuration saves");
    check(stlxconf_load(&g_reloaded, saved_path) == 0, "the saved file loads");
    check(holds_main_values(&g_reloaded), "a save writes the options back");

    char command[600];
    snprintf(command, sizeof(command), "rm -rf %s", g_dir);
    if (system(command) != 0) {
        printf("screenshot: cannot remove %s\n", g_dir);
    }

    if (g_failures != 0) {
        printf("screenshot: %llu of %llu checks failed\n", (unsigned long long)g_failures,
               (unsigned long long)g_checks);
        return 1;
    }

    printf("screenshot: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
