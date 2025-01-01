#ifndef CPIO_H
#define CPIO_H
#include <types.h>

#define CPIO_HEADER_MAGIC "070701"
#define CPIO_TRAILER_MARK "TRAILER!!!"

namespace fs {
struct cpio_newc_header {
    char c_magic[6];       // "070701"
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];    // size of the file in bytes
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];    // includes NULL terminator
    char c_check[8];
};

uint32_t cpio_from_hex_str(const char* str, size_t len);
bool cpio_is_dir(uint32_t mode);

__PRIVILEGED_CODE
void load_cpio_initrd(
    const uint8_t* cpio_archive,
    size_t length,
    const char* mount_path
);
} // namespace fs

#endif // CPIO_H
