/* Quick Actions' skip list and the file each launcher and action came from, run on the build machine: the default,
 * the main file's list kept against drop-ins, a save writing the list back, and each item's source path */

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
static char       g_dir[] = "/tmp/stlxconf-sources-XXXXXX";
static char       g_main_path[256];
static char       g_saved_path[256];
static char       g_drop_in_dir[256];
static char       g_drop_in_path[512];

static const char MAIN_TEXT[] =
    "[quick_actions]\n"
    "skip=/dev /proc /tmp\n"
    "\n"
    "[taskbar:terminal]\n"
    "label=Terminal\n"
    "path=/bin/stlxterm\n"
    "\n"
    "[action:about]\n"
    "label=About this computer\n"
    "action=exec\n"
    "path=/bin/stlxsettings\n";

static const char BROWSER_TEXT[] =
    "[quick_actions]\n"
    "skip=/\n"
    "\n"
    "[taskbar:browser]\n"
    "label=Browser\n"
    "path=/usr/bin/chrome\n"
    "\n"
    "[action:new_window]\n"
    "label=New Browser Window\n"
    "action=exec\n"
    "path=/usr/bin/chrome\n";

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

static void check_skip(void) {
    stlxconf_defaults(&g_conf);
    check(strcmp(g_conf.quick_actions_skip, STLXCONF_DEFAULT_QUICK_ACTIONS_SKIP) == 0,
          "the skip list starts as the default");

    check(stlxconf_load(&g_conf, g_main_path) == 0, "the main file loads");
    check(stlxconf_load_drop_ins(&g_conf, g_drop_in_dir) == 0, "the drop-in files load");
    check(strcmp(g_conf.quick_actions_skip, "/dev /proc /tmp") == 0, "the main file sets the skip list");

    check(stlxconf_save(&g_conf, g_saved_path) == 0, "the configuration saves");
    check(stlxconf_load(&g_reloaded, g_saved_path) == 0, "the saved file loads");
    check(strcmp(g_reloaded.quick_actions_skip, "/dev /proc /tmp") == 0, "a save writes the skip list back");
}

static void check_sources(void) {
    check(g_conf.pin_count == 2 && g_conf.action_count == 2, "every file's launchers and actions are read");
    check(strcmp(g_conf.pins[0].source_path, g_main_path) == 0, "a launcher names the main file it came from");
    check(strcmp(g_conf.pins[1].source_path, g_drop_in_path) == 0, "a launcher names the drop-in it came from");
    check(strcmp(g_conf.actions[0].source_path, g_main_path) == 0, "an action names the main file it came from");
    check(strcmp(g_conf.actions[1].source_path, g_drop_in_path) == 0, "an action names the drop-in it came from");
    check(g_reloaded.pin_count == 1 && strcmp(g_reloaded.pins[0].source_path, g_saved_path) == 0,
          "a launcher written back names the file it was read from again");
}

int main(void) {
    if (!mkdtemp(g_dir)) {
        printf("sources: cannot make a scratch directory\n");
        return 1;
    }

    snprintf(g_main_path, sizeof(g_main_path), "%s/stlxdm.conf", g_dir);
    snprintf(g_saved_path, sizeof(g_saved_path), "%s/saved.conf", g_dir);
    snprintf(g_drop_in_dir, sizeof(g_drop_in_dir), "%s/conf.d", g_dir);
    snprintf(g_drop_in_path, sizeof(g_drop_in_path), "%s/browser.conf", g_drop_in_dir);
    mkdir(g_drop_in_dir, 0755);

    write_file(g_main_path, MAIN_TEXT);
    write_file(g_drop_in_path, BROWSER_TEXT);

    check_skip();
    check_sources();

    char command[600];
    snprintf(command, sizeof(command), "rm -rf %s", g_dir);
    if (system(command) != 0) {
        printf("sources: cannot remove %s\n", g_dir);
    }

    if (g_failures != 0) {
        printf("sources: %llu of %llu checks failed\n", (unsigned long long)g_failures,
               (unsigned long long)g_checks);
        return 1;
    }

    printf("sources: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
