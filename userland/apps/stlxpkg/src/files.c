#define _POSIX_C_SOURCE 200809L

#include "files.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIRECTORY_MODE 0755

static int make_directory(const char* path) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        return errno == ENOENT ? mkdir(path, DIRECTORY_MODE) : -1;
    }

    if (!S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    return 0;
}

int make_parent_directories(const char* path) {
    char partial[PATH_MAX];
    size_t length = strlen(path);
    if (length >= sizeof(partial)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(partial, path, length + 1);
    for (size_t i = 1; i < length; i++) {
        if (partial[i] != '/') {
            continue;
        }

        partial[i] = '\0';
        int rc = make_directory(partial);
        partial[i] = '/';
        if (rc != 0) {
            return -1;
        }
    }

    return 0;
}

int make_directories(const char* path) {
    return make_parent_directories(path) == 0 ? make_directory(path) : -1;
}

/* Reads the first child of `path` into `child`. The directory is opened anew for every child, since a directory's
 * read position counts entries and deleting one would skip the next. */
static int first_child(const char* path, char* child, size_t size, bool* found) {
    DIR* directory = opendir(path);
    if (!directory) {
        return -1;
    }

    struct dirent* item = readdir(directory);
    while (item && (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)) {
        item = readdir(directory);
    }

    *found = item != NULL;
    int length = item ? snprintf(child, size, "%s/%s", path, item->d_name) : 0;
    closedir(directory);

    return length < (int)size ? 0 : -1;
}

int remove_tree(const char* path) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        return errno == ENOENT ? 0 : -1;
    }

    if (!S_ISDIR(status.st_mode)) {
        return unlink(path);
    }

    char child[PATH_MAX];
    bool found = true;
    while (found) {
        if (first_child(path, child, sizeof(child), &found) != 0) {
            return -1;
        }

        if (found && remove_tree(child) != 0) {
            return -1;
        }
    }

    return rmdir(path);
}
