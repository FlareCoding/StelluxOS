#ifndef STLXPKG_TAR_H
#define STLXPKG_TAR_H

/* A reader for the ustar archives packages ship, fed bytes as they arrive. It hands each regular file, directory
 * and symlink to its handlers and fails on any other entry type, so nothing but those three ever gets unpacked. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAR_BLOCK_SIZE 512
#define TAR_PATH_MAX   4096
#define TAR_PAX_MAX    (2 * TAR_PATH_MAX + 64)

enum {
    TAR_OK          = 0,
    TAR_ERR_HEADER  = -1, /* a header or pax record that is not well formed */
    TAR_ERR_TYPE    = -2, /* an entry that is not a regular file, a directory or a symlink */
    TAR_ERR_PATH    = -3, /* a path that is absolute, holds `..` or `.`, or has an empty component */
    TAR_ERR_HANDLER = -4, /* a handler failed and reported why */
};

typedef enum {
    TAR_FILE,
    TAR_DIRECTORY,
    TAR_SYMLINK,
} tar_entry_type;

typedef struct {
    tar_entry_type type;
    char           path[TAR_PATH_MAX];   /* relative, without the leading ./ or a trailing slash */
    char           target[TAR_PATH_MAX]; /* a symlink's target, as the archive states it */
    uint32_t       mode;
    uint64_t       size;
} tar_entry;

/* Handlers return 0 to go on. `entry` sees every entry but the archive's root, `data` a file's bytes in order and
 * `end_file` the end of each file's bytes. */
typedef struct {
    int (*entry)(void* context, const tar_entry* entry);
    int (*data)(void* context, const uint8_t* bytes, size_t length);
    int (*end_file)(void* context);
    void* context;
} tar_handlers;

typedef enum {
    TAR_READING_HEADER,
    TAR_READING_FILE,
    TAR_READING_PAX,
    TAR_SKIPPING,
    TAR_ENDED,
} tar_state;

typedef struct {
    tar_handlers handlers;
    tar_state    state;
    uint8_t      block[TAR_BLOCK_SIZE];
    size_t       block_length;
    uint64_t     remaining;              /* data bytes left of the current entry */
    uint64_t     padding;                /* bytes after them up to the next block */
    char         pax[TAR_PAX_MAX];       /* the pax records for the next entry */
    size_t       pax_length;
    tar_entry    entry;
} tar_reader;

void tar_init(tar_reader* reader, const tar_handlers* handlers);

/**
 * @brief Feeds the next `length` bytes of the archive. Bytes after its end-of-archive block are ignored.
 * @return TAR_OK or a negative TAR_ERR_* code.
 */
int tar_feed(tar_reader* reader, const uint8_t* bytes, size_t length);

bool tar_ended(const tar_reader* reader);

#endif /* STLXPKG_TAR_H */
