#ifndef STLXPKG_REMOVE_H
#define STLXPKG_REMOVE_H

/* Removes what a package installed, by the path lines of its record */

#include <stdbool.h>

/**
 * @brief Deletes each path of `lines` that `kept` does not hold, files and symlinks first, then directories deepest
 * first, keeping a directory that still holds anything. Missing paths are skipped, and `kept` may be NULL.
 * @return 0, or -1 once the failure is printed. `changed_drop_ins` becomes true when a deleted path is a drop-in.
 */
int remove_paths(const char* lines, const char* kept, bool* changed_drop_ins);

/**
 * @brief Removes package `name` by its record, then the record, without touching the network.
 * @return 0, or -1 once the failure is printed.
 */
int remove_package(const char* name);

#endif /* STLXPKG_REMOVE_H */
