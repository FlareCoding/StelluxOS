#ifndef STELLUX_SYSCALL_HANDLERS_SYS_ERROR_MAP_H
#define STELLUX_SYSCALL_HANDLERS_SYS_ERROR_MAP_H

#include "syscall/syscall_table.h"
#include "fs/fs.h"
#include "resource/resource.h"

namespace syscall::error_map {

inline int64_t map_socket_op_error(int32_t rc) {
    switch (rc) {
    case resource::ERR_INVAL:       return syscall::EINVAL;
    case resource::ERR_NOMEM:       return syscall::ENOMEM;
    case resource::ERR_ADDRINUSE:   return syscall::EADDRINUSE;
    case resource::ERR_CONNREFUSED: return syscall::ECONNREFUSED;
    case resource::ERR_ISCONN:      return syscall::EISCONN;
    case resource::ERR_NOTCONN:     return syscall::ENOTCONN;
    case resource::ERR_AGAIN:       return syscall::EAGAIN;
    case resource::ERR_NOENT:       return syscall::ENOENT;
    case resource::ERR_ACCESS:      return syscall::EACCES;
    case resource::ERR_NOTDIR:      return syscall::ENOTDIR;
    case resource::ERR_INTR:        return syscall::ERESTARTSYS;
    case resource::ERR_NOPROTOOPT:  return syscall::ENOPROTOOPT;
    case resource::ERR_MSGSIZE:     return syscall::EMSGSIZE;
    case resource::ERR_HOSTUNREACH: return syscall::EHOSTUNREACH;
    case resource::ERR_UNSUP:       return syscall::EOPNOTSUPP;
    default:                        return syscall::EIO;
    }
}

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
        case fs::ERR_AGAIN:
            return syscall::EAGAIN;
        case fs::ERR_XDEV:
            return syscall::EXDEV;
        case fs::ERR_IO:
        default:
            return syscall::EIO;
    }
}

} // namespace syscall::error_map

#endif // STELLUX_SYSCALL_HANDLERS_SYS_ERROR_MAP_H
