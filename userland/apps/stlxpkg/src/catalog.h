#ifndef STLXPKG_CATALOG_H
#define STLXPKG_CATALOG_H

/* What the newest package release offers this machine: the release's tag, read from the redirect of the latest
 * release's SHA256SUMS, and the lines of that release's SHA256SUMS for the machine's architecture */

#include <stddef.h>

#define CATALOG_FILE_MAX    192
#define CATALOG_TAG_MAX     64
#define CATALOG_SHA256_HEX  64
#define CATALOG_ENTRIES_MAX 128

/* Every part of an archive's name fits the name's own size */
typedef struct {
    char name[CATALOG_FILE_MAX];
    char version[CATALOG_FILE_MAX]; /* version-release, as packages.lock writes it */
    char arch[CATALOG_FILE_MAX];
    char sha256[CATALOG_SHA256_HEX + 1];
    char file[CATALOG_FILE_MAX];
} catalog_entry;

typedef struct {
    char          tag[CATALOG_TAG_MAX];
    catalog_entry entries[CATALOG_ENTRIES_MAX];
    size_t        count;
} catalog;

/**
 * @brief Finds the newest package release and reads what it offers the architecture `arch`.
 * @return 0, or -1 once the failure is printed.
 */
int catalog_load(catalog* catalog, const char* arch);

/**
 * @brief The entry of package `name`, or NULL when the release does not offer it.
 */
const catalog_entry* catalog_find(const catalog* catalog, const char* name);

/**
 * @brief Writes the URL of `entry`'s archive in the catalog's release to `url`.
 */
void catalog_archive_url(const catalog* catalog, const catalog_entry* entry, char* url, size_t size);

#endif /* STLXPKG_CATALOG_H */
