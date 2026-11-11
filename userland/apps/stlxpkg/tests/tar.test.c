/* The tar reader, run on the build machine over archives built here: the entries a package holds, a pax path past
 * the header's 100 characters at every split of the bytes, and the entries and paths it must refuse */

#include "tar.h"

#include <stdio.h>
#include <string.h>

#define MAX_REPORTS  8
#define ARCHIVE_MAX  (32 * TAR_BLOCK_SIZE)
#define SEEN_MAX     4096
#define LONG_NAME    "share/a-name-that-runs-well-past-the-one-hundred-characters-a-ustar-header-holds-for-its-name.txt"

typedef struct {
    uint8_t bytes[ARCHIVE_MAX];
    size_t  length;
} archive;

static uint64_t g_checks;
static uint64_t g_failures;
static archive  g_archive;
static char     g_seen[SEEN_MAX];
static char     g_data[SEEN_MAX];
static char     g_refused_path[TAR_PATH_MAX];

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

static void add_header(archive* a, const char* name, char type, unsigned mode, size_t size, const char* target) {
    char* header = (char*)a->bytes + a->length;
    memset(header, 0, TAR_BLOCK_SIZE);
    snprintf(header, 100, "%s", name);
    snprintf(header + 100, 8, "%07o", mode);
    snprintf(header + 124, 12, "%011o", (unsigned)size);

    header[156] = type;
    snprintf(header + 157, 100, "%s", target);
    memcpy(header + 257, "ustar\0" "00", 8);

    unsigned sum = 8 * ' ';
    for (size_t i = 0; i < TAR_BLOCK_SIZE; i++) {
        sum += (uint8_t)header[i];
    }

    snprintf(header + 148, 8, "%06o", sum);
    header[155] = ' ';
    a->length += TAR_BLOCK_SIZE;
}

static void add_entry(archive* a, const char* name, char type, unsigned mode, const char* data, const char* target) {
    size_t size = strlen(data);
    add_header(a, name, type, mode, size, target);
    memset(a->bytes + a->length, 0, (size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE * TAR_BLOCK_SIZE);
    memcpy(a->bytes + a->length, data, size);
    a->length += (size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE * TAR_BLOCK_SIZE;
}

/* A pax record "<length> <key>=<value>\n", whose length counts its own digits */
static void add_pax_path(archive* a, const char* path) {
    char record[TAR_PATH_MAX];
    size_t body = strlen(" path=\n") + strlen(path);
    size_t length = body + 1;
    while (length != body + (size_t)snprintf(NULL, 0, "%zu", length)) {
        length++;
    }

    snprintf(record, sizeof(record), "%zu path=%s\n", length, path);
    add_entry(a, "./PaxHeaders/long", 'x', 0644, record, "");
}

static void add_end(archive* a) {
    memset(a->bytes + a->length, 0, 2 * TAR_BLOCK_SIZE);
    a->length += 2 * TAR_BLOCK_SIZE;
}

static void append(char* log, const char* text) {
    strncat(log, text, SEEN_MAX - strlen(log) - 1);
}

static int on_entry(void* context, const tar_entry* entry) {
    (void)context;
    char line[TAR_PATH_MAX];
    snprintf(line, sizeof(line), "%c %s %o %s\n", "fdl"[entry->type], entry->path, entry->mode, entry->target);
    append(g_seen, line);

    return strcmp(entry->path, g_refused_path) == 0 ? -1 : 0;
}

static int on_data(void* context, const uint8_t* bytes, size_t length) {
    (void)context;
    strncat(g_data, (const char*)bytes, length);
    return 0;
}

static int on_end_file(void* context) {
    (void)context;
    append(g_seen, "= ");
    append(g_seen, g_data);
    append(g_seen, "\n");
    g_data[0] = '\0';
    return 0;
}

/* Feeds the archive in pieces of `piece` bytes and returns the first error, with what the handlers saw in g_seen */
static int read_archive(const archive* a, size_t piece, bool* ended) {
    static tar_reader reader;
    tar_handlers handlers = { on_entry, on_data, on_end_file, NULL };
    tar_init(&reader, &handlers);
    g_seen[0] = '\0';
    g_data[0] = '\0';

    for (size_t offset = 0; offset < a->length; offset += piece) {
        size_t size = a->length - offset < piece ? a->length - offset : piece;
        int rc = tar_feed(&reader, a->bytes + offset, size);
        if (rc != TAR_OK) {
            return rc;
        }
    }

    *ended = tar_ended(&reader);
    return TAR_OK;
}

static void check_package(void) {
    archive* a = &g_archive;
    a->length = 0;

    add_entry(a, "./", '5', 0755, "", "");
    add_entry(a, "./bin/", '5', 0755, "", "");
    add_entry(a, "./bin/tool", '2', 0777, "", "/usr/bin/tool");
    add_entry(a, "./usr/", '5', 0755, "", "");
    add_entry(a, "./usr/bin/tool", '0', 0755, "hello", "");

    add_pax_path(a, "./usr/" LONG_NAME);
    add_entry(a, "./usr/share/a-name-that-runs", '0', 0644, "long", "");
    add_entry(a, "./usr/short", '0', 0600, "", "");

    add_end(a);
    memcpy(a->bytes + a->length, "after the end", 13);
    a->length += 13;

    const char* expected = "d bin 755 \nl bin/tool 777 /usr/bin/tool\nd usr 755 \nf usr/bin/tool 755 \n= hello\n"
                           "f usr/" LONG_NAME " 644 \n= long\nf usr/short 600 \n= \n";
    for (size_t piece = 1; piece <= a->length; piece = piece < 64 ? piece + 1 : piece * 2) {
        bool ended = false;
        int rc = read_archive(a, piece, &ended);
        check(rc == TAR_OK && ended && strcmp(g_seen, expected) == 0, "a package archive reads the same at any split");
    }
}

static int read_single_entry(const char* name, char type) {
    bool ended = false;
    g_archive.length = 0;
    add_entry(&g_archive, name, type, 0644, "", type == '2' || type == '1' ? "target" : "");
    add_end(&g_archive);

    return read_archive(&g_archive, TAR_BLOCK_SIZE, &ended);
}

static void check_refusals(void) {
    check(read_single_entry("../etc/passwd", '0') == TAR_ERR_PATH, "a path climbing out with .. is refused");
    check(read_single_entry("/etc/passwd", '0') == TAR_ERR_PATH, "an absolute path is refused");
    check(read_single_entry("./usr//bin", '5') == TAR_ERR_PATH, "an empty component is refused");
    check(read_single_entry("./usr/./bin", '5') == TAR_ERR_PATH, "a . component is refused");
    check(read_single_entry("./", '0') == TAR_ERR_PATH, "a root that is not a directory is refused");

    check(read_single_entry("./usr/link", '1') == TAR_ERR_TYPE, "a hard link is refused");
    check(read_single_entry("./dev/tty", '3') == TAR_ERR_TYPE, "a device is refused");

    read_single_entry("./usr/bin/tool", '0');
    g_archive.bytes[0] ^= 1;
    bool ended = false;
    check(read_archive(&g_archive, TAR_BLOCK_SIZE, &ended) == TAR_ERR_HEADER, "a header failing its checksum");

    snprintf(g_refused_path, sizeof(g_refused_path), "usr/bin/tool");
    check(read_single_entry("./usr/bin/tool", '0') == TAR_ERR_HANDLER, "a handler's failure stops the archive");
    g_refused_path[0] = '\0';

    g_archive.length = 0;
    add_entry(&g_archive, "./usr/", '5', 0755, "", "");
    check(read_archive(&g_archive, TAR_BLOCK_SIZE, &ended) == TAR_OK && !ended, "an archive cut short has no end");
}

int main(void) {
    check_package();
    check_refusals();

    if (g_failures != 0) {
        printf("tar: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("tar: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
