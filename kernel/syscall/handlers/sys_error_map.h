#ifndef STELLUX_SYSCALL_HANDLERS_SYS_ERROR_MAP_H
#define STELLUX_SYSCALL_HANDLERS_SYS_ERROR_MAP_H

#include "syscall/syscall_table.h"
#include "fs/fs.h"

namespace syscall::error_map {

inline int64_t map_fs_error(int32_t rc) {
    switch (rc) {
        case fs::ERR_NOENT:
            return syscall::ENOENT;
        case fs::ERR_EXIST:
            return syscall::EEXIST;
        case fs::ERR_NOTDIR:
            return syscall::ENOTDIR;
        case fs::ERR_ISDIR:
            return syscall::EISDIR;
        case fs::ERR_NOMEM:
            return syscall::ENOMEM;
        case fs::ERR_INVAL:
            return syscall::EINVAL;
        case fs::ERR_NAMETOOLONG:
            return syscall::ENAMETOOLONG;
        case fs::ERR_NOTEMPTY:
            return syscall::ENOTEMPTY;
        case fs::ERR_NOSYS:
            return syscall::ENOSYS;
        case fs::ERR_BUSY:
            return syscall::EBUSY;
        case fs::ERR_LOOP:
            return syscall::ELOOP;
        case fs::ERR_BADF:
            return syscall::EBADF;
        case fs::ERR_IO:
        default:
            return syscall::EIO;
    }
}

} // namespace syscall::error_map

#endif // STELLUX_SYSCALL_HANDLERS_SYS_ERROR_MAP_H
