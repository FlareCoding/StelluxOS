#ifndef STLXPKG_FILES_H
#define STLXPKG_FILES_H

/* Directory helpers that never follow a symlink, so nothing an install writes can land outside the path it names */

/**
 * @brief Creates each missing directory above `path`.
 * @return 0, or -1 with errno set, ENOTDIR when one of them exists as something else.
 */
int make_parent_directories(const char* path);

/**
 * @brief Creates the directory `path` and each missing directory above it, as make_parent_directories does.
 */
int make_directories(const char* path);

/**
 * @brief Deletes `path` and everything below it.
 * @return 0, also when `path` does not exist, or -1 with errno set.
 */
int remove_tree(const char* path);

#endif /* STLXPKG_FILES_H */
