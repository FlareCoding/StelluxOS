/* The clipboard history's limits, run on the build machine: the defaults, the main file's values kept against
 * drop-ins, and a save writing them back */

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
static char       g_dir[] = "/tmp/stlxconf-clipboard-XXXXXX";

static const char MAIN_TEXT[] =
    "[clipboard]\n"
    "history_items=25\n"
    "history_mb=8\n";

static const char DROP_IN_TEXT[] =
    "[clipboard]\n"
    "history_items=1000\n";

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

int main(void) {
    if (!mkdtemp(g_dir)) {
        printf("clipboard: cannot make a scratch directory\n");
        return 1;
    }

    char main_path[256];
    char saved_path[256];
    char drop_in_dir[256];
    char drop_in_path[512];

    snprintf(main_path, sizeof(main_path), "%s/stlxdm.conf", g_dir);
    snprintf(saved_path, sizeof(saved_path), "%s/saved.conf", g_dir);
    snprintf(drop_in_dir, sizeof(drop_in_dir), "%s/conf.d", g_dir);
    snprintf(drop_in_path, sizeof(drop_in_path), "%s/greedy.conf", drop_in_dir);

    mkdir(drop_in_dir, 0755);
    write_file(main_path, MAIN_TEXT);
    write_file(drop_in_path, DROP_IN_TEXT);

    stlxconf_defaults(&g_conf);
    check(g_conf.clipboard_history_items == STLXCONF_DEFAULT_CLIPBOARD_ITEMS &&
          g_conf.clipboard_history_mb == STLXCONF_DEFAULT_CLIPBOARD_MB, "the limits start as the defaults");

    check(stlxconf_load(&g_conf, main_path) == 0, "the main file loads");
    check(stlxconf_load_drop_ins(&g_conf, drop_in_dir) == 0, "the drop-in files load");
    check(g_conf.clipboard_history_items == 25 && g_conf.clipboard_history_mb == 8,
          "the main file sets the limits and a drop-in cannot");

    check(stlxconf_save(&g_conf, saved_path) == 0, "the configuration saves");
    check(stlxconf_load(&g_reloaded, saved_path) == 0, "the saved file loads");
    check(g_reloaded.clipboard_history_items == 25 && g_reloaded.clipboard_history_mb == 8,
          "a save writes the limits back");

    char command[600];
    snprintf(command, sizeof(command), "rm -rf %s", g_dir);
    if (system(command) != 0) {
        printf("clipboard: cannot remove %s\n", g_dir);
    }

    if (g_failures != 0) {
        printf("clipboard: %llu of %llu checks failed\n", (unsigned long long)g_failures,
               (unsigned long long)g_checks);
        return 1;
    }

    printf("clipboard: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
