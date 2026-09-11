#ifndef STELLUX_RESOURCE_SOCKET_OPS_H
#define STELLUX_RESOURCE_SOCKET_OPS_H

#include "resource/resource.h"

namespace resource {

using sendto_fn = ssize_t (*)(resource_object* obj, const void* ksrc, size_t count,
                              uint32_t flags, const void* kaddr, size_t addrlen);
using recvfrom_fn = ssize_t (*)(resource_object* obj, void* kdst, size_t count,
                                uint32_t flags, void* kaddr, size_t* addrlen);
using bind_fn = int32_t (*)(resource_object* obj, const void* kaddr, size_t addrlen);
using listen_fn = int32_t (*)(resource_object* obj, int32_t backlog);
using accept_fn = int32_t (*)(resource_object* obj, resource_object** new_obj,
                              void* kaddr, size_t* addrlen, bool nonblock);
using connect_fn = int32_t (*)(resource_object* obj, const void* kaddr, size_t addrlen);
using getname_fn = int32_t (*)(resource_object* obj, void* kaddr, size_t* addrlen, bool peer);
using setsockopt_fn = int32_t (*)(resource_object* obj, int32_t level,
                                  int32_t optname, const void* optval, size_t optlen);
using getsockopt_fn = int32_t (*)(resource_object* obj, int32_t level,
                                  int32_t optname, void* optval, size_t* optlen);
using shutdown_fn = int32_t (*)(resource_object* obj, int32_t how);

/**
 * Operations only sockets have. Every entry is nullable, the syscall layer
 * reports EOPNOTSUPP for a missing one. `getname` returns the local address,
 * or the peer's when `peer` is set.
 */
struct socket_ops {
    bind_fn       bind = nullptr;
    listen_fn     listen = nullptr;
    accept_fn     accept = nullptr;
    connect_fn    connect = nullptr;
    sendto_fn     sendto = nullptr;
    recvfrom_fn   recvfrom = nullptr;
    getname_fn    getname = nullptr;
    setsockopt_fn setsockopt = nullptr;
    getsockopt_fn getsockopt = nullptr;
    shutdown_fn   shutdown = nullptr;
};

// The socket operations of `obj`, or nullptr when it is not a socket
inline const socket_ops* socket_ops_of(const resource_object* obj) {
    return obj && obj->ops ? obj->ops->socket : nullptr;
}

} // namespace resource

#endif // STELLUX_RESOURCE_SOCKET_OPS_H
