#ifndef STELLUX_SYSCALL_RESOURCE_ERRNO_H
#define STELLUX_SYSCALL_RESOURCE_ERRNO_H

#include "syscall/syscall_table.h"
#include "resource/resource.h"

namespace syscall {

inline int64_t map_resource_error(int64_t rc) {
    switch (rc) {
        case resource::ERR_INVAL:
            return EINVAL;
        case resource::ERR_NOENT:
            return ENOENT;
        case resource::ERR_NOTDIR:
            return ENOTDIR;
        case resource::ERR_NAMETOOLONG:
            return ENAMETOOLONG;
        case resource::ERR_NOMEM:
            return ENOMEM;
        case resource::ERR_TABLEFULL:
            return EMFILE;
        case resource::ERR_AGAIN:
            return EAGAIN;
        case resource::ERR_PIPE:
            return EPIPE;
        case resource::ERR_ADDRINUSE:
            return EADDRINUSE;
        case resource::ERR_AFNOSUPPORT:
            return EAFNOSUPPORT;
        case resource::ERR_PROTONOSUPPORT:
            return EPROTONOSUPPORT;
        case resource::ERR_NOTCONN:
            return ENOTCONN;
        case resource::ERR_CONNREFUSED:
            return ECONNREFUSED;
        case resource::ERR_OPNOTSUPP:
            return EOPNOTSUPP;
        case resource::ERR_NOTSOCK:
            return ENOTSOCK;
        case resource::ERR_BADF:
        case resource::ERR_ACCESS:
            return EBADF;
        case resource::ERR_UNSUP:
            return ENOSYS;
        case resource::ERR_IO:
        default:
            return EIO;
    }
}

} // namespace syscall

#endif // STELLUX_SYSCALL_RESOURCE_ERRNO_H
