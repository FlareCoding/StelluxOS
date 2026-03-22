#ifndef STELLUX_FS_DEVFS_DEVFS_H
#define STELLUX_FS_DEVFS_DEVFS_H

#include "common/types.h"

namespace fs { class node; }

namespace devfs {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

/**
 * @brief Register the devfs driver with the VFS.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Add a char_device node to the devfs root directory.
 * Must be called after devfs is mounted. The node's parent and
 * filesystem are set automatically.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t add_char_device(const char* name, fs::node* dev_node);

/**
 * @brief Ensure a subdirectory exists under the devfs root.
 * Creates it if it does not already exist. Returns a pointer
 * to the directory node on success.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE fs::node* ensure_dir(const char* name);

/**
 * @brief Add a char_device node under a specific devfs directory.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t add_char_device_at(fs::node* dir, fs::node* dev_node);

} // namespace devfs

#endif // STELLUX_FS_DEVFS_DEVFS_H
