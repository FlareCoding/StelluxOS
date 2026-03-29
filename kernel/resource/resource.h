#ifndef STELLUX_RESOURCE_RESOURCE_H
#define STELLUX_RESOURCE_RESOURCE_H

#include "resource/resource_types.h"
#include "resource/handle_table.h"
#include "rc/ref_counted.h"

namespace sched { struct task; }
namespace mm { struct mm_context; }
namespace sync { struct poll_table; }

namespace resource {

struct resource_object;

using read_fn = ssize_t (*)(resource_object* obj, void* kdst, size_t count, uint32_t flags);
using write_fn = ssize_t (*)(resource_object* obj, const void* ksrc, size_t count, uint32_t flags);
using close_fn = void (*)(resource_object* obj);
using ioctl_fn = int32_t (*)(resource_object* obj, uint32_t cmd, uint64_t arg);
using mmap_fn = int32_t (*)(resource_object* obj, mm::mm_context* mm_ctx,
                            uintptr_t addr, size_t length, uint32_t prot,
                            uint32_t map_flags, uint64_t offset, uintptr_t* out_addr);
using sendto_fn = ssize_t (*)(resource_object* obj, const void* ksrc, size_t count,
                              uint32_t flags, const void* kaddr, size_t addrlen);
using recvfrom_fn = ssize_t (*)(resource_object* obj, void* kdst, size_t count,
                                uint32_t flags, void* kaddr, size_t* addrlen);
using bind_fn = int32_t (*)(resource_object* obj, const void* kaddr, size_t addrlen);
using listen_fn = int32_t (*)(resource_object* obj, int32_t backlog);
using accept_fn = int32_t (*)(resource_object* obj, resource_object** new_obj,
                              void* kaddr, size_t* addrlen, bool nonblock);
using connect_fn = int32_t (*)(resource_object* obj, const void* kaddr, size_t addrlen);
using setsockopt_fn = int32_t (*)(resource_object* obj, int32_t level,
                                  int32_t optname, const void* optval, size_t optlen);
using getsockopt_fn = int32_t (*)(resource_object* obj, int32_t level,
                                  int32_t optname, void* optval, size_t* optlen);
using poll_fn = uint32_t (*)(resource_object* obj, sync::poll_table* pt);

struct resource_ops {
    read_fn     read;
    write_fn    write;
    close_fn    close;
    ioctl_fn    ioctl;       // nullable
    mmap_fn     mmap;        // nullable
    sendto_fn   sendto;      // nullable — for datagram/raw sockets
    recvfrom_fn recvfrom;    // nullable — for datagram/raw sockets
    bind_fn     bind;        // nullable — for sockets
    listen_fn   listen;      // nullable — for stream sockets
    accept_fn   accept;      // nullable — for listening sockets
    connect_fn  connect;     // nullable — for stream sockets
    setsockopt_fn setsockopt; // nullable — for sockets
    getsockopt_fn getsockopt; // nullable — for sockets
    poll_fn       poll;       // nullable — returns readiness mask, subscribes on wait queues
};

struct resource_object : rc::ref_counted<resource_object> {
    resource_type type;
    const resource_ops* ops;
    void* impl;

    /**
     * @brief Finalize and free a resource object at terminal release.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(resource_object* self);
};

constexpr int32_t OK            = 0;
constexpr int32_t ERR_INVAL     = -1;
constexpr int32_t ERR_NOENT     = -2;
constexpr int32_t ERR_NOMEM     = -3;
constexpr int32_t ERR_BADF      = -4;
constexpr int32_t ERR_ACCESS    = -5;
constexpr int32_t ERR_IO        = -6;
constexpr int32_t ERR_TABLEFULL = -7;
constexpr int32_t ERR_UNSUP     = -8;
constexpr int32_t ERR_NOTDIR    = -9;
constexpr int32_t ERR_NAMETOOLONG = -10;
constexpr int32_t ERR_PIPE        = -11;
constexpr int32_t ERR_NOTCONN     = -12;
constexpr int32_t ERR_CONNREFUSED = -13;
constexpr int32_t ERR_ADDRINUSE   = -14;
constexpr int32_t ERR_ISCONN      = -15;
constexpr int32_t ERR_AGAIN       = -16;
constexpr int32_t ERR_EXIST       = -17;
constexpr int32_t ERR_INTR        = -18;
constexpr int32_t ERR_NOPROTOOPT  = -19;

/**
 * @brief Initialize handle table storage in task.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_task_handles(sched::task* task);

/**
 * @brief Open path-backed resource and install a handle in task table.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t open(
    sched::task* owner,
    const char* kpath,
    uint32_t flags,
    handle_t* out_handle
);

/**
 * @brief Read from handle into kernel buffer.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t read(
    sched::task* owner,
    handle_t handle,
    void* kdst,
    size_t count
);

/**
 * @brief Write to handle from kernel buffer.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE ssize_t write(
    sched::task* owner,
    handle_t handle,
    const void* ksrc,
    size_t count
);

/**
 * @brief Invoke ioctl on a handle.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t ioctl(
    sched::task* owner,
    handle_t handle,
    uint32_t cmd,
    uint64_t arg
);

/**
 * @brief Close one handle from task table.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t close(
    sched::task* owner,
    handle_t handle
);

/**
 * @brief Close all handles owned by task.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void close_all(sched::task* owner);

/**
 * @brief Increment resource object reference.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void resource_add_ref(resource_object* obj);

/**
 * @brief Decrement resource object reference and destroy on last ref.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void resource_release(resource_object* obj);

} // namespace resource

#endif // STELLUX_RESOURCE_RESOURCE_H
