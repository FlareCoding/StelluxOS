#define _POSIX_C_SOURCE 200809L

#include "records.h"
#include "files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOCK_PATH       STATE_DIR "/lock"
#define NEW_RECORD_PATH STATE_DIR "/record.new"
#define HEADER_MAX      (4 * CATALOG_FILE_MAX)
#define HEADER_FIELDS   4

static char g_path[PATH_MAX];

int records_lock(void) {
    if (make_directories(RECORDS_DIR) != 0) {
        fprintf(stderr, "stlxpkg: cannot create %s (errno %d)\n", RECORDS_DIR, errno);
        return -1;
    }

    int fd = open(LOCK_PATH, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        fprintf(stderr, "stlxpkg: cannot open %s (errno %d)\n", LOCK_PATH, errno);
        return -1;
    }

    struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(fd, F_SETLK, &lock) != 0) {
        fprintf(stderr, "stlxpkg: another stlxpkg is running\n");
        return -1;
    }

    return 0;
}

/* Splits "<name> <version> <arch> <sha256>" into the entry's fields */
static bool parse_header(const char* line, catalog_entry* installed) {
    char* fields[HEADER_FIELDS] = { installed->name, installed->version, installed->arch, installed->sha256 };
    size_t sizes[HEADER_FIELDS] = {
        sizeof(installed->name), sizeof(installed->version), sizeof(installed->arch), sizeof(installed->sha256),
    };

    for (size_t i = 0; i < HEADER_FIELDS; i++) {
        size_t length = strcspn(line, " \n");
        if (length == 0 || length >= sizes[i]) {
            return false;
        }

        memcpy(fields[i], line, length);
        fields[i][length] = '\0';
        line += length + (line[length] == ' ' ? 1 : 0);
    }

    installed->file[0] = '\0';

    return true;
}

static int read_header(const char* path, catalog_entry* installed) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return errno == ENOENT ? 0 : -1;
    }

    char line[HEADER_MAX];
    bool parsed = fgets(line, sizeof(line), file) && parse_header(line, installed);
    fclose(file);

    return parsed ? 1 : -1;
}

int record_read(const char* name, catalog_entry* installed) {
    snprintf(g_path, sizeof(g_path), "%s/%s", RECORDS_DIR, name);
    int found = read_header(g_path, installed);
    if (found < 0) {
        fprintf(stderr, "stlxpkg: cannot read the record %s\n", g_path);
    }

    return found;
}

int record_write(const catalog_entry* entry, const char* paths, size_t length) {
    FILE* file = fopen(NEW_RECORD_PATH, "w");
    bool written = file != NULL;
    if (file) {
        fprintf(file, "%s %s %s %s\n", entry->name, entry->version, entry->arch, entry->sha256);
        fwrite(paths, 1, length, file);
        written = !ferror(file);
        written = fclose(file) == 0 && written;
    }

    snprintf(g_path, sizeof(g_path), "%s/%s", RECORDS_DIR, entry->name);
    if (!written || rename(NEW_RECORD_PATH, g_path) != 0) {
        fprintf(stderr, "stlxpkg: cannot write the record %s (errno %d)\n", g_path, errno);
        unlink(NEW_RECORD_PATH);
        return -1;
    }

    return 0;
}

static bool record_lists(const char* path, const char* line) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return false;
    }

    char* text = NULL;
    size_t capacity = 0;
    bool listed = false;
    while (!listed && getline(&text, &capacity, file) > 0) {
        text[strcspn(text, "\n")] = '\0';
        listed = strcmp(text, line) == 0;
    }

    free(text);
    fclose(file);

    return listed;
}

bool record_find_owner(const char* line, char* owner, size_t size) {
    DIR* directory = opendir(RECORDS_DIR);
    if (!directory) {
        return false;
    }

    bool found = false;
    struct dirent* item = readdir(directory);
    while (item && !found) {
        snprintf(g_path, sizeof(g_path), "%s/%s", RECORDS_DIR, item->d_name);
        found = item->d_name[0] != '.' && record_lists(g_path, line);
        if (found) {
            snprintf(owner, size, "%s", item->d_name);
        }

        item = readdir(directory);
    }

    closedir(directory);

    return found;
}

int records_list(catalog_entry* installed, size_t capacity) {
    DIR* directory = opendir(RECORDS_DIR);
    if (!directory) {
        fprintf(stderr, "stlxpkg: cannot read %s (errno %d)\n", RECORDS_DIR, errno);
        return -1;
    }

    size_t count = 0;
    struct dirent* item = readdir(directory);
    while (item && count < capacity) {
        snprintf(g_path, sizeof(g_path), "%s/%s", RECORDS_DIR, item->d_name);
        if (item->d_name[0] != '.' && read_header(g_path, &installed[count]) == 1) {
            count++;
        }

        item = readdir(directory);
    }

    closedir(directory);

    return (int)count;
}
