#ifndef STLXPKG_DESKTOP_H
#define STLXPKG_DESKTOP_H

/* Packages register with the desktop by shipping drop-in files, which it reads at start and again on a reload signal */

#include <stdbool.h>

/**
 * @brief Whether `path` is a drop-in file the desktop reads.
 */
bool desktop_reads(const char* path);

/**
 * @brief Asks a running desktop to reload its drop-ins, as Settings does after a change. Without one it does nothing.
 */
void desktop_reload(void);

#endif /* STLXPKG_DESKTOP_H */
