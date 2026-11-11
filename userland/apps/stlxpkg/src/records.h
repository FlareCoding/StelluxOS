#ifndef STLXPKG_RECORDS_H
#define STLXPKG_RECORDS_H

/* One record per package in /var/lib/stlxpkg/records: a first line "<name> <version>-<release> <arch> <sha256>" as in
 * packages.lock, then each absolute path the package installed, a directory's ending in a slash. */

#include "catalog.h"

#include <stdbool.h>
#include <stddef.h>

#define STATE_DIR   "/var/lib/stlxpkg"
#define RECORDS_DIR STATE_DIR "/records"

/**
 * @brief Creates the state directory and takes the lock every run holds until it exits.
 * @return 0, or -1 once the failure is printed, as when another run holds the lock.
 */
int records_lock(void);

/**
 * @brief Reads the first line of package `name`'s record into `installed`, whose file stays empty.
 * @return 1 when the package is installed, 0 when it is not, or -1 once the failure is printed.
 */
int record_read(const char* name, catalog_entry* installed);

/**
 * @brief Writes `entry`'s record with the path lines `paths`, replacing the old record in one rename.
 * @return 0, or -1 once the failure is printed.
 */
int record_write(const catalog_entry* entry, const char* paths, size_t length);

/**
 * @brief Finds the package whose record lists `line`, a path as records write it.
 * @return whether one does, its name then in `owner`.
 */
bool record_find_owner(const char* line, char* owner, size_t size);

/**
 * @brief Reads the first line of every record, at most `capacity` of them.
 * @return the number read, or -1 once the failure is printed.
 */
int records_list(catalog_entry* installed, size_t capacity);

#endif /* STLXPKG_RECORDS_H */
