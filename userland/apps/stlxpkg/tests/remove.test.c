/* remove_paths, run on the build machine in a temporary tree: what an older release alone installed goes, a directory
 * still holding anything stays, missing paths are skipped, and a kept list spares exactly the lines it holds */
#define _POSIX_C_SOURCE 200809L

#include "records.h"
#include "remove.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_REPORTS 8
#define LINES_MAX   4096
#define PATH_SIZE   256

static uint64_t g_checks;
static uint64_t g_failures;
static char     g_root[PATH_SIZE];
static char     g_old[LINES_MAX];
static char     g_new[LINES_MAX];

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

static const char* under_root(const char* path) {
    static char full[PATH_SIZE];
    snprintf(full, sizeof(full), "%s%s", g_root, path);
    return full;
}

static void add_line(char* lines, const char* path) {
    strncat(lines, under_root(path), LINES_MAX - strlen(lines) - 1);
    strncat(lines, "\n", LINES_MAX - strlen(lines) - 1);
}

/* Creates `path` under the root, a directory when it ends in a slash */
static void create(const char* path) {
    if (path[strlen(path) - 1] == '/') {
        mkdir(under_root(path), 0755);
        return;
    }

    FILE* file = fopen(under_root(path), "w");
    fclose(file);
}

static bool exists(const char* path) {
    struct stat status;
    return lstat(under_root(path), &status) == 0;
}

static void check_line_matching(void) {
    check(record_paths_hold("/a/\n/lib/x\n", "/lib/x"), "a line matches itself");
    check(!record_paths_hold("/usr/lib/x\n", "/lib/x"), "a line ending another does not match");
    check(!record_paths_hold("/lib/x.old\n", "/lib/x"), "a line starting another does not match");
    check(record_paths_hold("/lib/xy\n/lib/x\n", "/lib/x"), "a line matches past a longer one");
    check(!record_paths_hold("/lib/x/\n", "/lib/x"), "a file does not match a directory of its name");
}

static void check_upgrade(void) {
    const char* old_paths[] = { "/a/", "/a/kept", "/a/kept.old", "/a/sub/", "/a/sub/gone", "/a/missing", "/shared/" };
    const char* new_paths[] = { "/a/", "/a/kept", "/a/new" };
    for (size_t i = 0; i < sizeof(old_paths) / sizeof(old_paths[0]); i++) {
        add_line(g_old, old_paths[i]);
        create(old_paths[i]);
    }

    for (size_t i = 0; i < sizeof(new_paths) / sizeof(new_paths[0]); i++) {
        add_line(g_new, new_paths[i]);
        create(new_paths[i]);
    }

    unlink(under_root("/a/missing"));
    create("/shared/foreign");

    bool changed_drop_ins = false;
    check(remove_paths(g_old, g_new, &changed_drop_ins) == 0, "deleting an older release's leftovers succeeds");
    check(!changed_drop_ins, "no drop-in changed");

    check(exists("/a/kept") && exists("/a/new") && exists("/a/"), "what the new release lists stays");
    check(!exists("/a/kept.old"), "a path whose name extends a kept one goes");
    check(!exists("/a/sub/gone") && !exists("/a/sub/"), "a file goes before its emptied directory");
    check(exists("/shared/foreign"), "a directory holding another file stays with it");
}

static void check_remove(void) {
    bool changed_drop_ins = false;
    check(remove_paths(g_new, NULL, &changed_drop_ins) == 0, "removing a package succeeds");
    check(!exists("/a/"), "removing a package empties and deletes its directories");

    g_old[0] = '\0';
    add_line(g_old, "/now-a-directory");
    create("/now-a-directory/");
    check(remove_paths(g_old, NULL, &changed_drop_ins) != 0, "a file that became a directory stops the removal");
}

int main(void) {
    snprintf(g_root, sizeof(g_root), "/tmp/stlxpkg-remove-%d", (int)getpid());
    if (mkdir(g_root, 0755) != 0) {
        printf("remove: cannot create %s\n", g_root);
        return 1;
    }

    check_line_matching();
    check_upgrade();
    check_remove();

    rmdir(under_root("/now-a-directory"));
    unlink(under_root("/shared/foreign"));
    rmdir(under_root("/shared"));
    rmdir(g_root);

    if (g_failures != 0) {
        printf("remove: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("remove: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
