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

/**
 * @brief Converts a hexadecimal string to a 32-bit unsigned integer.
 * @param str Pointer to the hexadecimal string.
 * @param len Length of the string to convert.
 * @return The 32-bit unsigned integer representation of the hexadecimal string.
 * 
 * Interprets the provided string as a hexadecimal number and converts it to a numerical value.
 */
uint32_t cpio_from_hex_str(const char* str, size_t len);

/**
 * @brief Checks whether a CPIO archive entry represents a directory.
 * @param mode The mode field of the CPIO archive entry.
 * @return True if the mode indicates a directory, false otherwise.
 * 
 * Evaluates the mode field of a CPIO entry to determine if it corresponds to a directory.
 */
bool cpio_is_dir(uint32_t mode);

/**
 * @brief Loads an initrd (initial RAM disk) from a CPIO archive.
 * @param cpio_archive Pointer to the start of the CPIO archive.
 * @param length Length of the CPIO archive in bytes.
 * @param mount_path Path where the archive should be mounted or extracted.
 * 
 * Parses and processes the provided CPIO archive, loading its contents into the specified mount path.
 * The loaded contents are stored in a newly created and mounted RAM filesystem.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void load_cpio_initrd(
    const uint8_t* cpio_archive,
    size_t length,
    const char* mount_path
);
} // namespace fs

#endif // CPIO_H
