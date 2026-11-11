/* Action sections, run on the build machine: read from the main file and drop-in files with every field, written back
 * by a save without the drop-ins' entries, and bounded by the table's size */

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
static char       g_dir[] = "/tmp/stlxconf-action-XXXXXX";
static char       g_main_path[256];
static char       g_saved_path[256];
static char       g_drop_in_dir[256];

static const char MAIN_TEXT[] =
    "[action:displays]\n"
    "label=Displays\n"
    "subtitle=Settings: where each display sits\n"
    "keywords=monitor resolution refresh\n"
    "icon=/etc/res/icons/settings-32.png\n"
    "action=exec\n"
    "path=/bin/stlxsettings\n"
    "args=--page displays\n"
    "\n"
    "[action:terminal]\n"
    "label=Terminal\n"
    "action=exec\n"
    "path=/bin/stlxterm\n";

static const char SHEEN_TEXT[] =
    "[action:new_text_file]\n"
    "label=New Text File\n"
    "action=exec\n"
    "path=/usr/bin/sheen\n";

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

static void write_file(const char* dir, const char* name, const char* text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    FILE* file = fopen(path, "w");
    if (!file) {
        check(false, path);
        return;
    }

    fputs(text, file);
    fclose(file);
}

static void remove_file(const char* dir, const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    unlink(path);
}

static bool is_displays(const stlxconf_action_t* action) {
    return strcmp(action->name, "displays") == 0 && strcmp(action->label, "Displays") == 0 &&
           strcmp(action->subtitle, "Settings: where each display sits") == 0 &&
           strcmp(action->keywords, "monitor resolution refresh") == 0 &&
           strcmp(action->icon_path, "/etc/res/icons/settings-32.png") == 0 && strcmp(action->action, "exec") == 0 &&
           strcmp(action->path, "/bin/stlxsettings") == 0 && strcmp(action->args, "--page displays") == 0;
}

static bool is_terminal(const stlxconf_action_t* action) {
    return strcmp(action->name, "terminal") == 0 && strcmp(action->label, "Terminal") == 0 &&
           strcmp(action->action, "exec") == 0 && strcmp(action->path, "/bin/stlxterm") == 0 &&
           action->args[0] == '\0' && action->subtitle[0] == '\0' && action->keywords[0] == '\0' &&
           action->icon_path[0] == '\0';
}

static void check_read(void) {
    write_file(g_dir, "stlxdm.conf", MAIN_TEXT);
    write_file(g_drop_in_dir, "10-sheen.conf", SHEEN_TEXT);

    check(stlxconf_load(&g_conf, g_main_path) == 0, "the main file loads");
    check(stlxconf_load_drop_ins(&g_conf, g_drop_in_dir) == 0, "the drop-in files load");

    check(g_conf.action_count == 3, "every file's actions are read");
    check(is_displays(&g_conf.actions[0]), "an action keeps every field");
    check(is_terminal(&g_conf.actions[1]), "an action needs only a label, an action and a program");
    check(strcmp(g_conf.actions[2].path, "/usr/bin/sheen") == 0, "a drop-in's action follows the main file's");
    check(!g_conf.actions[0].from_drop_in && g_conf.actions[2].from_drop_in,
          "an action is marked by the file it came from");
}

static void check_save(void) {
    check(stlxconf_save(&g_conf, g_saved_path) == 0, "the configuration saves");
    check(stlxconf_load(&g_reloaded, g_saved_path) == 0, "the saved file loads");

    check(g_reloaded.action_count == 2, "only the main file's actions are written back");
    check(is_displays(&g_reloaded.actions[0]), "a written action keeps every field");
    check(is_terminal(&g_reloaded.actions[1]), "a written action leaves out the fields it lacks");
}

static void check_table_bound(void) {
    char drop_in_text[96 * (STLXCONF_MAX_ACTIONS + 2)] = "";
    size_t used = 0;
    for (int i = 0; i < STLXCONF_MAX_ACTIONS + 2; i++) {
        used += (size_t)snprintf(drop_in_text + used, sizeof(drop_in_text) - used,
                                 "[action:a%d]\nlabel=A%d\naction=exec\npath=/bin/a%d\n", i, i, i);
    }

    remove_file(g_drop_in_dir, "10-sheen.conf");
    write_file(g_dir, "stlxdm.conf", "");
    write_file(g_drop_in_dir, "30-many.conf", drop_in_text);

    stlxconf_load(&g_conf, g_main_path);
    stlxconf_load_drop_ins(&g_conf, g_drop_in_dir);

    const stlxconf_action_t* last = &g_conf.actions[STLXCONF_MAX_ACTIONS - 1];
    check(g_conf.action_count == STLXCONF_MAX_ACTIONS, "actions past the table are dropped");
    check(strcmp(last->label, "A31") == 0 && strcmp(last->path, "/bin/a31") == 0,
          "the last action that fits keeps its values");
}

int main(void) {
    if (!mkdtemp(g_dir)) {
        printf("action: cannot make a scratch directory\n");
        return 1;
    }

    snprintf(g_main_path, sizeof(g_main_path), "%s/stlxdm.conf", g_dir);
    snprintf(g_saved_path, sizeof(g_saved_path), "%s/saved.conf", g_dir);
    snprintf(g_drop_in_dir, sizeof(g_drop_in_dir), "%s/conf.d", g_dir);
    mkdir(g_drop_in_dir, 0755);

    check_read();
    check_save();
    check_table_bound();

    remove_file(g_drop_in_dir, "30-many.conf");
    rmdir(g_drop_in_dir);
    remove_file(g_dir, "stlxdm.conf");
    remove_file(g_dir, "saved.conf");
    rmdir(g_dir);

    if (g_failures != 0) {
        printf("action: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("action: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
