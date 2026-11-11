#ifndef STLXPKG_INSTALL_H
#define STLXPKG_INSTALL_H

/* Installs a package as one transaction: its archive streams through SHA-256 and zstd into a staging tree, and its
 * paths move into place only once every byte of it checked out */

#include "catalog.h"

/**
 * @brief Installs package `name` from the catalog's release, doing nothing when its record already holds the same
 * SHA-256. A failure before the first path moves leaves the system as it was.
 * @return 0, or -1 once the failure is printed.
 */
int install_package(const catalog* catalog, const char* name);

#endif /* STLXPKG_INSTALL_H */
