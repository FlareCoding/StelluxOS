/* File types and the handler lookup, run on the build machine: types by extension in any case and by content without
 * one, each type's parents up to its root, and the handler of the nearest type that has one */

#include <stlxconf/conf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_REPORTS 8

typedef struct {
    const char* name;
    const char* bytes;
    size_t      length;
    const char* mime_type;
    const char* description;
} sample_file_t;

typedef struct {
    const char* name;
    const char* handler_path; /* NULL when nothing opens it */
    const char* handled_type;
} expected_opener_t;

static const sample_file_t SAMPLES[] = {
    { "notes.txt", "hello\n", 6, "text/plain", "Plain text" },
    { "main.c", "int x;\n", 7, "text/x-csrc", "C source" },
    { "util.HPP", "#pragma once\n", 13, "text/x-c++hdr", "C++ header" },
    { "stlxdm.conf", "[desktop]\n", 10, "text/plain", "Configuration" },
    { "photo.png", "not really", 10, "image/png", "PNG image" },
    { "README", "Read me first.\n", 15, "text/plain", "Plain text" },
    { ".bashrc", "export A=1\n", 11, "text/plain", "Plain text" },
    { "trailing.", "dot at the end\n", 15, "text/plain", "Plain text" },
    { "accents", "h\xc3\xa9llo w\xc3\xb6rld\n", 14, "text/plain", "Plain text" },
    { "empty", "", 0, "text/plain", "Plain text" },
    { "prog", "\x7f" "ELF\x02\x01\x01\x00\x00", 9, "application/x-executable", "Program" },
    { "run", "#!/bin/sh\necho hi\n", 18, "application/x-shellscript", "Script" },
    { "picture", "\x89PNG\r\n\x1a\n", 8, "image/png", "PNG image" },
    { "blob", "ab\x00\x01" "cd", 6, "application/octet-stream", "Binary data" },
    { "latin1", "caf\xe9 au lait\n", 13, "application/octet-stream", "Binary data" },
};

static const char* const HPP_CHAIN[] = { "text/x-c++hdr", "text/x-chdr", "text/x-csrc", "text/plain", NULL };

static const char CONF_TEXT[] =
    "[file_handler:text/plain]\n"
    "path=/usr/bin/sheen\n"
    "\n"
    "[file_handler:text/x-csrc]\n"
    "path=/bin/vim\n";

static const expected_opener_t OPENERS[] = {
    { "main.c", "/bin/vim", "text/x-csrc" },
    { "util.HPP", "/bin/vim", "text/x-csrc" },
    { "notes.txt", "/usr/bin/sheen", "text/plain" },
    { "README", "/usr/bin/sheen", "text/plain" },
    { "run", "/usr/bin/sheen", "text/plain" },
    { "empty", "/usr/bin/sheen", "text/plain" },
    { "photo.png", NULL, NULL },
    { "prog", NULL, NULL },
    { "blob", NULL, NULL },
};

static uint64_t   g_checks;
static uint64_t   g_failures;
static stlxconf_t g_conf;
static char       g_dir[] = "/tmp/stlxconf-type-XXXXXX";

static void check(bool holds, const char* what, const char* subject) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s: %s\n", what, subject);
    }
}

static void sample_path(char* path, size_t size, const char* name) {
    snprintf(path, size, "%s/%s", g_dir, name);
}

static void write_bytes(const char* name, const char* bytes, size_t length) {
    char path[512];
    sample_path(path, sizeof(path), name);

    FILE* file = fopen(path, "wb");
    if (!file) {
        check(false, "the sample can be written", name);
        return;
    }

    fwrite(bytes, 1, length, file);
    fclose(file);
}

static void check_types(void) {
    for (size_t i = 0; i < sizeof(SAMPLES) / sizeof(SAMPLES[0]); i++) {
        const sample_file_t* sample = &SAMPLES[i];
        char path[512];
        sample_path(path, sizeof(path), sample->name);

        stlxconf_file_type_t type = stlxconf_file_type(path);
        check(strcmp(type.mime_type, sample->mime_type) == 0, "has its type", sample->name);
        check(strcmp(type.description, sample->description) == 0, "has its description", sample->name);
    }
}

static void check_parents(void) {
    const char* type = HPP_CHAIN[0];
    for (size_t i = 1; i < sizeof(HPP_CHAIN) / sizeof(HPP_CHAIN[0]); i++) {
        const char* parent = stlxconf_parent_type(type);
        bool is_expected = HPP_CHAIN[i] ? parent && strcmp(parent, HPP_CHAIN[i]) == 0 : parent == NULL;
        check(is_expected, "derives from the next type of its chain", type);
        type = parent;
    }

    const char* markdown = stlxconf_parent_type("text/markdown");
    check(markdown && strcmp(markdown, "text/plain") == 0, "a text type derives from text/plain", "text/markdown");
    check(stlxconf_parent_type("image/png") == NULL, "an image derives from nothing", "image/png");
    check(stlxconf_parent_type("TEXT/PLAIN") == NULL, "text/plain is a root, compared without case", "TEXT/PLAIN");
}

static void check_openers(void) {
    char conf_path[512];
    sample_path(conf_path, sizeof(conf_path), "stlxdm-test.conf");
    FILE* file = fopen(conf_path, "w");
    if (file) {
        fputs(CONF_TEXT, file);
        fclose(file);
    }

    check(stlxconf_load(&g_conf, conf_path) == 0, "the configuration loads", conf_path);
    for (size_t i = 0; i < sizeof(OPENERS) / sizeof(OPENERS[0]); i++) {
        const expected_opener_t* expected = &OPENERS[i];
        char path[512];
        sample_path(path, sizeof(path), expected->name);

        const char* handled = NULL;
        const char* type = stlxconf_file_type(path).mime_type;
        const stlxconf_file_handler_t* handler = stlxconf_find_file_opener(&g_conf, type, &handled);
        if (!expected->handler_path) {
            check(handler == NULL && handled == NULL, "opens in nothing", expected->name);
            continue;
        }

        check(handler && strcmp(handler->path, expected->handler_path) == 0, "opens in its handler", expected->name);
        check(handled && strcmp(handled, expected->handled_type) == 0, "names the type it opens as", expected->name);
    }
}

int main(void) {
    if (!mkdtemp(g_dir)) {
        printf("file_type: cannot make a scratch directory\n");
        return 1;
    }

    for (size_t i = 0; i < sizeof(SAMPLES) / sizeof(SAMPLES[0]); i++) {
        write_bytes(SAMPLES[i].name, SAMPLES[i].bytes, SAMPLES[i].length);
    }

    check_types();
    check_parents();
    check_openers();

    char command[600];
    snprintf(command, sizeof(command), "rm -rf %s", g_dir);
    if (system(command) != 0) {
        printf("file_type: cannot remove %s\n", g_dir);
    }

    if (g_failures != 0) {
        printf("file_type: %llu of %llu checks failed\n", (unsigned long long)g_failures,
               (unsigned long long)g_checks);
        return 1;
    }

    printf("file_type: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
