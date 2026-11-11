#define _POSIX_C_SOURCE 200809L

#include "remove.h"
#include "desktop.h"
#include "records.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_line[PATH_MAX + 1];

/* Copies the line `length` long at `line` into g_line, unless `kept` holds it */
static bool take_line(const char* line, size_t length, const char* kept) {
    if (length == 0 || length >= sizeof(g_line)) {
        return false;
    }

    memcpy(g_line, line, length);
    g_line[length] = '\0';

    return !kept || !record_paths_hold(kept, g_line);
}

static int remove_file(bool* changed_drop_ins) {
    if (unlink(g_line) != 0 && errno != ENOENT) {
        fprintf(stderr, "stlxpkg: cannot delete %s (errno %d)\n", g_line, errno);
        return -1;
    }

    *changed_drop_ins = *changed_drop_ins || desktop_reads(g_line);

    return 0;
}

/* A directory is kept while it holds something, so only other failures of rmdir count */
static int remove_directory(void) {
    if (rmdir(g_line) != 0 && errno != ENOTEMPTY && errno != EEXIST && errno != EBUSY && errno != ENOENT) {
        fprintf(stderr, "stlxpkg: cannot delete %s (errno %d)\n", g_line, errno);
        return -1;
    }

    return 0;
}

static int remove_files(const char* lines, const char* kept, bool* changed_drop_ins) {
    const char* line = lines;
    while (*line != '\0') {
        size_t length = strcspn(line, "\n");
        bool is_directory = length > 0 && line[length - 1] == '/';
        if (!is_directory && take_line(line, length, kept) && remove_file(changed_drop_ins) != 0) {
            return -1;
        }

        line += line[length] == '\n' ? length + 1 : length;
    }

    return 0;
}

/* A record lists each directory before what it holds, so walking it backwards empties every directory first */
static int remove_directories(const char* lines, const char* kept) {
    const char* end = lines + strlen(lines);
    while (end > lines) {
        const char* line_end = end[-1] == '\n' ? end - 1 : end;
        const char* line = line_end;
        while (line > lines && line[-1] != '\n') {
            line--;
        }

        size_t length = (size_t)(line_end - line);
        bool is_directory = length > 1 && line[length - 1] == '/';
        if (is_directory && take_line(line, length, kept)) {
            g_line[length - 1] = '\0';
            if (remove_directory() != 0) {
                return -1;
            }
        }

        end = line;
    }

    return 0;
}

int remove_paths(const char* lines, const char* kept, bool* changed_drop_ins) {
    if (remove_files(lines, kept, changed_drop_ins) != 0) {
        return -1;
    }

    return remove_directories(lines, kept);
}

int remove_package(const char* name) {
    catalog_entry installed;
    char* paths = NULL;
    int found = record_read(name, &installed, &paths);
    if (found == 0) {
        fprintf(stderr, "stlxpkg: %s is not installed\n", name);
    }

    if (found != 1) {
        return -1;
    }

    bool changed_drop_ins = false;
    int rc = remove_paths(paths, NULL, &changed_drop_ins);
    free(paths);
    if (rc == 0) {
        rc = record_delete(name);
    }

    if (changed_drop_ins) {
        desktop_reload();
    }

    if (rc == 0) {
        printf("stlxpkg: removed %s %s\n", name, installed.version);
    }

    return rc;
}
