#ifndef STELLUX_FS_FS_H
#define STELLUX_FS_FS_H

#include "fs/fstypes.h"

namespace fs {

class node;
class file;
struct driver;

constexpr int32_t OK            =  0;
constexpr int32_t ERR_NOENT     = -1;
constexpr int32_t ERR_EXIST     = -2;
constexpr int32_t ERR_NOTDIR    = -3;
constexpr int32_t ERR_ISDIR     = -4;
constexpr int32_t ERR_NOMEM     = -5;
constexpr int32_t ERR_INVAL     = -6;
constexpr int32_t ERR_NAMETOOLONG = -7;
constexpr int32_t ERR_NOTEMPTY  = -8;
constexpr int32_t ERR_NOSYS     = -9;
constexpr int32_t ERR_IO        = -10;
constexpr int32_t ERR_BUSY      = -11;
constexpr int32_t ERR_LOOP      = -12;
constexpr int32_t ERR_BADF      = -13;

/**
 * @brief Initialize the filesystem subsystem. Registers ramfs,
 * mounts empty rootfs at /.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Register a filesystem driver.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_driver(driver* drv);

/**
 * @brief Mount a filesystem at a target path.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t mount(const char* source, const char* target,
                                const char* fs_name, uint32_t flags);

/**
 * @brief Unmount a filesystem at a target path.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t unmount(const char* target);

// --- Consumer API ---

file* open(const char* path, uint32_t flags);
file* open(const char* path, uint32_t flags, int32_t* out_err);
ssize_t read(file* f, void* buf, size_t count);
ssize_t write(file* f, const void* buf, size_t count);
int64_t seek(file* f, int64_t offset, int whence);
int32_t close(file* f);
int32_t ioctl(file* f, uint32_t cmd, uint64_t arg);

int32_t stat(const char* path, vattr* attr);
int32_t fstat(file* f, vattr* attr);

int32_t mkdir(const char* path, uint32_t mode);
int32_t rmdir(const char* path);
int32_t unlink(const char* path);
ssize_t readdir(file* f, dirent* entries, size_t count);

/**
 * @brief Resolve a path to a node.
 * On success, the returned node has add_ref() called.
 * Caller must release() when done (or adopt into a strong_ref).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t lookup(const char* path, node** out);

} // namespace fs

#endif // STELLUX_FS_FS_H
