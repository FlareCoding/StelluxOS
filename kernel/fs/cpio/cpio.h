#ifndef STELLUX_FS_CPIO_CPIO_H
#define STELLUX_FS_CPIO_CPIO_H

#include "common/types.h"

namespace cpio {

constexpr int32_t OK                = 0;
constexpr int32_t ERR_NO_MODULE     = -1;
constexpr int32_t ERR_BAD_ARCHIVE   = -2;
constexpr int32_t ERR_MAP_FAILED    = -3;

struct cpio_newc_header {
    char c_magic[6];
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
};

/**
 * Load initrd from the first Limine boot module into the ramfs at /initrd/.
 * Maps the module's physical pages via vmm::map_phys(), parses the CPIO
 * newc archive, extracts files and directories, then unmaps.
 * Called from fs::init() when modules are present.
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t load_initrd();

} // namespace cpio

#endif // STELLUX_FS_CPIO_CPIO_H
