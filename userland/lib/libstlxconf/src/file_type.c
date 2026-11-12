/* File types by a name's extension or a file's first bytes, and the handler lookup that walks a type's parents, so
 * stlxdm and Qt apps open a file in the same program */
#include <stlxconf/conf.h>

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Enough of a file to tell its magic number and whether it reads as text */
#define SNIFF_BYTES 512

typedef struct {
    const char*          extension;
    stlxconf_file_type_t type;
} extension_type_t;

static const extension_type_t EXTENSION_TYPES[] = {
    { "txt", { "text/plain", "Plain text" } },
    { "conf", { "text/plain", "Configuration" } },
    { "cfg", { "text/plain", "Configuration" } },
    { "ini", { "text/plain", "Configuration" } },
    { "log", { "text/plain", "Log" } },
    { "md", { "text/markdown", "Markdown" } },
    { "c", { "text/x-csrc", "C source" } },
    { "h", { "text/x-chdr", "C header" } },
    { "cpp", { "text/x-c++src", "C++ source" } },
    { "cc", { "text/x-c++src", "C++ source" } },
    { "cxx", { "text/x-c++src", "C++ source" } },
    { "hpp", { "text/x-c++hdr", "C++ header" } },
    { "hh", { "text/x-c++hdr", "C++ header" } },
    { "py", { "text/x-python3", "Python script" } },
    { "sh", { "application/x-shellscript", "Shell script" } },
    { "json", { "application/json", "JSON" } },
    { "xml", { "application/xml", "XML document" } },
    { "html", { "text/html", "HTML document" } },
    { "htm", { "text/html", "HTML document" } },
    { "css", { "text/css", "CSS stylesheet" } },
    { "js", { "text/javascript", "JavaScript" } },
    { "png", { "image/png", "PNG image" } },
    { "jpg", { "image/jpeg", "JPEG image" } },
    { "jpeg", { "image/jpeg", "JPEG image" } },
    { "gif", { "image/gif", "GIF image" } },
    { "bmp", { "image/bmp", "BMP image" } },
    { "svg", { "image/svg+xml", "SVG image" } },
    { "webp", { "image/webp", "WebP image" } },
    { "pdf", { "application/pdf", "PDF document" } },
    { "ttf", { "font/ttf", "TrueType font" } },
    { "otf", { "font/otf", "OpenType font" } },
    { "tar", { "application/x-tar", "Tar archive" } },
    { "gz", { "application/gzip", "Gzip archive" } },
    { "zip", { "application/zip", "Zip archive" } },
};

typedef struct {
    const char*          magic;
    size_t               length;
    stlxconf_file_type_t type;
} magic_type_t;

static const magic_type_t MAGIC_TYPES[] = {
    { "\x7f" "ELF", 4, { "application/x-executable", "Program" } },
    { "\x89PNG", 4, { "image/png", "PNG image" } },
    { "\xff\xd8\xff", 3, { "image/jpeg", "JPEG image" } },
    { "GIF8", 4, { "image/gif", "GIF image" } },
    { "#!", 2, { "application/x-shellscript", "Script" } },
};

/* The parents shared-mime-info names where a type's family does not already say it, since every text type
 * derives from text/plain */
static const struct {
    const char* type;
    const char* parent;
} TYPE_PARENTS[] = {
    { "text/x-chdr", "text/x-csrc" },
    { "text/x-c++src", "text/x-csrc" },
    { "text/x-c++hdr", "text/x-chdr" },
    { "application/x-shellscript", "text/plain" },
    { "application/json", "text/plain" },
    { "application/xml", "text/plain" },
    { "image/svg+xml", "application/xml" },
};

static const stlxconf_file_type_t PLAIN_TEXT = { "text/plain", "Plain text" };
static const stlxconf_file_type_t BINARY_DATA = { "application/octet-stream", "Binary data" };

#define COUNT(table) (sizeof(table) / sizeof((table)[0]))

/* Whether `bytes` read as text: no NUL, and UTF-8 throughout but for a sequence the sample cuts short */
static bool is_text(const unsigned char* bytes, size_t length) {
    size_t at = 0;
    while (at < length) {
        unsigned char lead = bytes[at];
        size_t extra = lead < 0x80 ? 0 : (lead & 0xE0) == 0xC0 ? 1 : (lead & 0xF0) == 0xE0 ? 2
                     : (lead & 0xF8) == 0xF0 ? 3 : SIZE_MAX;
        if (lead == 0 || extra == SIZE_MAX) {
            return false;
        }

        if (at + extra >= length) {
            return true;
        }

        for (size_t i = 1; i <= extra; i++) {
            if ((bytes[at + i] & 0xC0) != 0x80) {
                return false;
            }
        }

        at += extra + 1;
    }

    return true;
}

/* Only a regular file is opened and read: a device, a pipe or a socket may block its reader or lose the bytes it gives,
 * so it types as data unread. The check repeats on the opened handle, since the path may change between the two. */
static stlxconf_file_type_t sniffed_type(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return BINARY_DATA;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return BINARY_DATA;
    }

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return BINARY_DATA;
    }

    unsigned char bytes[SNIFF_BYTES];
    ssize_t length = read(fd, bytes, sizeof(bytes));
    close(fd);
    if (length <= 0) {
        return length == 0 ? PLAIN_TEXT : BINARY_DATA;
    }

    for (size_t i = 0; i < COUNT(MAGIC_TYPES); i++) {
        const magic_type_t* magic = &MAGIC_TYPES[i];
        if ((size_t)length >= magic->length && memcmp(bytes, magic->magic, magic->length) == 0) {
            return magic->type;
        }
    }

    return is_text(bytes, (size_t)length) ? PLAIN_TEXT : BINARY_DATA;
}

/* A leading dot starts a hidden file's name rather than an extension */
stlxconf_file_type_t stlxconf_file_type(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    const char* dot = strrchr(name, '.');
    if (dot && dot != name) {
        for (size_t i = 0; i < COUNT(EXTENSION_TYPES); i++) {
            if (strcasecmp(dot + 1, EXTENSION_TYPES[i].extension) == 0) {
                return EXTENSION_TYPES[i].type;
            }
        }
    }

    return sniffed_type(path);
}

const char* stlxconf_parent_type(const char* mime_type) {
    for (size_t i = 0; i < COUNT(TYPE_PARENTS); i++) {
        if (strcasecmp(mime_type, TYPE_PARENTS[i].type) == 0) {
            return TYPE_PARENTS[i].parent;
        }
    }

    bool is_text = strncasecmp(mime_type, "text/", 5) == 0 && strcasecmp(mime_type, PLAIN_TEXT.mime_type) != 0;
    return is_text ? PLAIN_TEXT.mime_type : NULL;
}

const stlxconf_file_handler_t* stlxconf_find_file_opener(const stlxconf_t* conf, const char* mime_type,
                                                         const char** handled_type) {
    for (const char* type = mime_type; type; type = stlxconf_parent_type(type)) {
        const stlxconf_file_handler_t* handler = stlxconf_find_file_handler(conf, type);
        if (handler) {
            if (handled_type) {
                *handled_type = type;
            }

            return handler;
        }
    }

    return NULL;
}
