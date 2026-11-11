/* File handler sections, run on the build machine: read from the main file and drop-in files, found by MIME type
 * with the main file first, written back by a save, and bounded by the table's size */

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
static char       g_dir[] = "/tmp/stlxconf-test-XXXXXX";
static char       g_main_path[256];
static char       g_saved_path[256];
static char       g_drop_in_dir[256];

static const char MAIN_TEXT[] =
    "[desktop]\n"
    "wallpaper=/etc/res/wallpapers/ember-nebula.jpg\n"
    "\n"
    "[file_handler:text/x-csrc]\n"
    "path=/bin/stlxterm\n"
    "args=/bin/vim -R\n";

static const char SHEEN_TEXT[] =
    "[taskbar:sheen]\n"
    "label=Sheen\n"
    "path=/usr/bin/sheen\n"
    "\n"
    "[file_handler:text/plain]\n"
    "path=/usr/bin/sheen\n"
    "\n"
    "[file_handler:text/x-csrc]\n"
    "path=/usr/bin/sheen\n";

static const char VIEWER_TEXT[] =
    "[file_handler:IMAGE/PNG]\n"
    "path=/usr/bin/viewer\n"
    "args=--fit\n"
    "\n"
    "[file_handlers:text/html]\n"
    "path=/usr/bin/viewer\n";

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

static bool is_handler(const stlxconf_t* conf, const char* mime_type, const char* path, const char* args) {
    const stlxconf_file_handler_t* handler = stlxconf_find_file_handler(conf, mime_type);
    return handler && strcmp(handler->path, path) == 0 && strcmp(handler->args, args) == 0;
}

static void check_lookup(void) {
    write_file(g_dir, "stlxdm.conf", MAIN_TEXT);
    write_file(g_drop_in_dir, "10-sheen.conf", SHEEN_TEXT);
    write_file(g_drop_in_dir, "20-viewer.conf", VIEWER_TEXT);

    check(stlxconf_load(&g_conf, g_main_path) == 0, "the main file loads");
    check(stlxconf_load_drop_ins(&g_conf, g_drop_in_dir) == 0, "the drop-in files load");

    check(g_conf.file_handler_count == 4, "every file's handlers are read and a misspelled section is not");
    check(is_handler(&g_conf, "text/plain", "/usr/bin/sheen", ""), "a drop-in's handler is found by its type");
    check(is_handler(&g_conf, "text/x-csrc", "/bin/stlxterm", "/bin/vim -R"),
          "the main file's handler wins over a drop-in's");
    check(is_handler(&g_conf, "image/png", "/usr/bin/viewer", "--fit"), "types compare without case");

    check(stlxconf_find_file_handler(&g_conf, "text/markdown") == NULL, "a type no file names finds none");
    check(stlxconf_find_file_handler(&g_conf, "text") == NULL, "a part of a type finds none");
    check(!g_conf.file_handlers[0].from_drop_in && g_conf.file_handlers[1].from_drop_in,
          "a handler is marked by the file it came from");
}

static void check_save(void) {
    check(stlxconf_save(&g_conf, g_saved_path) == 0, "the configuration saves");
    check(stlxconf_load(&g_reloaded, g_saved_path) == 0, "the saved file loads");

    check(g_reloaded.file_handler_count == 1, "only the main file's handler is written back");
    check(is_handler(&g_reloaded, "text/x-csrc", "/bin/stlxterm", "/bin/vim -R"), "a written handler keeps its args");
}

/* A full URL handler table sits just before the file handlers, so values written past either end would show */
static void check_table_bound(void) {
    char main_text[96 * (STLXCONF_MAX_URL_HANDLERS + 1)] = "";
    size_t used = 0;
    for (int i = 0; i < STLXCONF_MAX_URL_HANDLERS; i++) {
        used += (size_t)snprintf(main_text + used, sizeof(main_text) - used, "[url_handler:s%d]\npath=/bin/u%d\n", i,
                                 i);
    }

    snprintf(main_text + used, sizeof(main_text) - used, "%s", "[file_handler:text/x-csrc]\npath=/bin/vim\n");

    char drop_in_text[96 * (STLXCONF_MAX_FILE_HANDLERS + 2)] = "";
    used = 0;
    for (int i = 0; i < STLXCONF_MAX_FILE_HANDLERS + 2; i++) {
        used += (size_t)snprintf(drop_in_text + used, sizeof(drop_in_text) - used,
                                 "[file_handler:text/x-test%d]\npath=/bin/t%d\nargs=--t%d\n", i, i, i);
    }

    remove_file(g_drop_in_dir, "10-sheen.conf");
    remove_file(g_drop_in_dir, "20-viewer.conf");
    write_file(g_dir, "stlxdm.conf", main_text);
    write_file(g_drop_in_dir, "30-many.conf", drop_in_text);

    stlxconf_load(&g_conf, g_main_path);
    stlxconf_load_drop_ins(&g_conf, g_drop_in_dir);

    check(g_conf.file_handler_count == STLXCONF_MAX_FILE_HANDLERS, "handlers past the table are dropped");
    check(is_handler(&g_conf, "text/x-test30", "/bin/t30", "--t30"), "the last handler that fits keeps its values");
    check(stlxconf_find_file_handler(&g_conf, "text/x-test31") == NULL, "a dropped handler is not found");
    check(is_handler(&g_conf, "text/x-csrc", "/bin/vim", ""), "a dropped handler's values reach no file handler");

    bool urls_kept = g_conf.url_handler_count == STLXCONF_MAX_URL_HANDLERS;
    for (int i = 0; i < STLXCONF_MAX_URL_HANDLERS; i++) {
        char scheme[16];
        char path[16];
        snprintf(scheme, sizeof(scheme), "s%d", i);
        snprintf(path, sizeof(path), "/bin/u%d", i);
        const stlxconf_url_handler_t* handler = stlxconf_find_url_handler(&g_conf, scheme);
        urls_kept = urls_kept && handler && strcmp(handler->path, path) == 0 && handler->args[0] == '\0';
    }

    check(urls_kept, "a dropped handler's values reach no URL handler");
}

int main(void) {
    if (!mkdtemp(g_dir)) {
        printf("file_handler: cannot make a scratch directory\n");
        return 1;
    }

    snprintf(g_main_path, sizeof(g_main_path), "%s/stlxdm.conf", g_dir);
    snprintf(g_saved_path, sizeof(g_saved_path), "%s/saved.conf", g_dir);
    snprintf(g_drop_in_dir, sizeof(g_drop_in_dir), "%s/conf.d", g_dir);
    mkdir(g_drop_in_dir, 0755);

    check_lookup();
    check_save();
    check_table_bound();

    remove_file(g_drop_in_dir, "30-many.conf");
    rmdir(g_drop_in_dir);
    remove_file(g_dir, "stlxdm.conf");
    remove_file(g_dir, "saved.conf");
    rmdir(g_dir);

    if (g_failures != 0) {
        printf("file_handler: %llu of %llu checks failed\n", (unsigned long long)g_failures,
               (unsigned long long)g_checks);
        return 1;
    }

    printf("file_handler: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
