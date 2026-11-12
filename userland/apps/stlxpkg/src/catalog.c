#include "catalog.h"
#include "report.h"

#include <stlxhttp/http.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SUMS_NAME      "SHA256SUMS"
#define SUMS_MAX       65536
#define TAG_PREFIX     "packages-"
#define ARCHIVE_SUFFIX ".tar.zst"
#define NAME_CHARS     "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-"

static char g_sums[SUMS_MAX + 1];
static char g_url[STLXHTTP_URL_MAX + 1];

static bool is_name(const char* text, size_t length) {
    return length > 0 && text[0] != '.' && strspn(text, NAME_CHARS) >= length;
}

/* The tag is the second to last part of the redirect's ".../download/<tag>/SHA256SUMS" */
static int read_tag(catalog* catalog, const char* location) {
    const char* suffix = "/" SUMS_NAME;
    size_t length = location ? strlen(location) : 0;
    size_t start = 0;
    size_t end = 0;
    if (length > strlen(suffix) && strcmp(location + length - strlen(suffix), suffix) == 0) {
        end = length - strlen(suffix);
        start = end;
        while (start > 0 && location[start - 1] != '/') {
            start--;
        }
    }

    size_t tag_length = end - start;
    if (tag_length == 0 || tag_length >= CATALOG_TAG_MAX || !is_name(location + start, tag_length)) {
        return fail(FAILURE_NETWORK, "the latest release does not redirect to a release's %s", SUMS_NAME);
    }

    memcpy(catalog->tag, location + start, tag_length);
    catalog->tag[tag_length] = '\0';

    return 0;
}

static int find_newest_tag(catalog* catalog) {
    stlxhttp_request* request = NULL;
    int32_t opened = stlxhttp_open(STLXPKG_LATEST_URL "/" SUMS_NAME, STLXHTTP_NO_FOLLOW, &request);
    int rc = opened == STLXHTTP_OK ? read_tag(catalog, stlxhttp_location(request)) :
                                     fail_http(opened, stlxhttp_error_message(request));
    stlxhttp_close(request);
    if (rc != 0) {
        return rc;
    }

    if (strncmp(catalog->tag, TAG_PREFIX, strlen(TAG_PREFIX)) != 0) {
        return fail(FAILURE_VERIFY, "the newest release, %s, is not a package release", catalog->tag);
    }

    return 0;
}

static int download_sums(const catalog* catalog, size_t* length) {
    snprintf(g_url, sizeof(g_url), "%s/%s/%s", STLXPKG_RELEASES_URL, catalog->tag, SUMS_NAME);
    stlxhttp_request* request = NULL;
    int32_t opened = stlxhttp_open(g_url, 0, &request);
    if (opened != STLXHTTP_OK) {
        fail_http(opened, stlxhttp_error_message(request));
        stlxhttp_close(request);
        return -1;
    }

    int64_t count = 0;
    *length = 0;
    while (*length < SUMS_MAX) {
        count = stlxhttp_read(request, g_sums + *length, SUMS_MAX - *length);
        if (count <= 0) {
            break;
        }

        *length += (size_t)count;
    }

    if (count < 0) {
        fail_http((int32_t)count, stlxhttp_error_message(request));
    } else if (*length == SUMS_MAX) {
        fail(FAILURE_VERIFY, "%s's %s is longer than %d bytes", catalog->tag, SUMS_NAME, SUMS_MAX);
    }

    stlxhttp_close(request);
    g_sums[*length] = '\0';

    return count < 0 || *length == SUMS_MAX ? -1 : 0;
}

/* Cuts `text` at its last dash and returns what followed, NULL when either side would be empty */
static char* cut_last_part(char* text) {
    char* dash = strrchr(text, '-');
    if (!dash || dash == text || dash[1] == '\0') {
        return NULL;
    }

    *dash = '\0';

    return dash + 1;
}

/* Splits "python-3.12.8-1-aarch64.tar.zst" from the right, as packages/publish.sh does, since names hold dashes */
static bool split_archive_name(const char* file, catalog_entry* entry) {
    size_t length = strlen(file);
    size_t suffix_length = strlen(ARCHIVE_SUFFIX);
    if (length <= suffix_length || length >= CATALOG_FILE_MAX || !is_name(file, length) ||
        strcmp(file + length - suffix_length, ARCHIVE_SUFFIX) != 0) {
        return false;
    }

    char stem[CATALOG_FILE_MAX];
    memcpy(stem, file, length - suffix_length);
    stem[length - suffix_length] = '\0';

    char* arch = cut_last_part(stem);
    char* release = arch ? cut_last_part(stem) : NULL;
    char* version = release ? cut_last_part(stem) : NULL;
    if (!version) {
        return false;
    }

    snprintf(entry->name, sizeof(entry->name), "%s", stem);
    snprintf(entry->version, sizeof(entry->version), "%s-%s", version, release);
    snprintf(entry->arch, sizeof(entry->arch), "%s", arch);
    snprintf(entry->file, sizeof(entry->file), "%s", file);

    return true;
}

/* Keeps a "<sha256>  <archive>" line when its archive is built for `arch` */
static bool take_sums_line(catalog* catalog, const char* line, const char* arch) {
    size_t hex_length = strspn(line, "0123456789abcdef");
    if (hex_length != CATALOG_SHA256_HEX || strncmp(line + hex_length, "  ", 2) != 0) {
        return false;
    }

    catalog_entry* entry = &catalog->entries[catalog->count];
    if (catalog->count == CATALOG_ENTRIES_MAX || !split_archive_name(line + hex_length + 2, entry)) {
        return false;
    }

    if (strcmp(entry->arch, arch) == 0) {
        memcpy(entry->sha256, line, CATALOG_SHA256_HEX);
        entry->sha256[CATALOG_SHA256_HEX] = '\0';
        catalog->count++;
    }

    return true;
}

int catalog_load(catalog* catalog, const char* arch) {
    size_t length = 0;
    catalog->count = 0;
    if (find_newest_tag(catalog) != 0 || download_sums(catalog, &length) != 0) {
        return -1;
    }

    char* line = g_sums;
    int line_number = 1;
    while (*line != '\0') {
        char* end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }

        if (*line != '\0' && !take_sums_line(catalog, line, arch)) {
            return fail(FAILURE_VERIFY, "line %d of %s's %s is malformed", line_number, catalog->tag, SUMS_NAME);
        }

        line = end ? end + 1 : line + strlen(line);
        line_number++;
    }

    return 0;
}

const catalog_entry* catalog_find(const catalog* catalog, const char* name) {
    for (size_t i = 0; i < catalog->count; i++) {
        if (strcmp(catalog->entries[i].name, name) == 0) {
            return &catalog->entries[i];
        }
    }

    return NULL;
}

void catalog_archive_url(const catalog* catalog, const catalog_entry* entry, char* url, size_t size) {
    snprintf(url, size, "%s/%s/%s", STLXPKG_RELEASES_URL, catalog->tag, entry->file);
}
