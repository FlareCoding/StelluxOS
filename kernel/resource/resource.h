#ifndef STELLUX_RESOURCE_RESOURCE_H
#define STELLUX_RESOURCE_RESOURCE_H

#include "resource/resource_types.h"
#include "resource/handle_table.h"
#include "rc/ref_counted.h"

namespace sched { struct task; }

namespace resource {

struct resource_object;

using read_fn = ssize_t (*)(resource_object* obj, void* kdst, size_t count);
using write_fn = ssize_t (*)(resource_object* obj, const void* ksrc, size_t count);
using close_fn = void (*)(resource_object* obj);

struct resource_ops {
    read_fn read;
    write_fn write;
    close_fn close;
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
constexpr int32_t ERR_AGAIN     = -11;
constexpr int32_t ERR_PIPE      = -12;
constexpr int32_t ERR_ADDRINUSE = -13;
constexpr int32_t ERR_AFNOSUPPORT = -14;
constexpr int32_t ERR_PROTONOSUPPORT = -15;
constexpr int32_t ERR_NOTCONN   = -16;
constexpr int32_t ERR_CONNREFUSED = -17;
constexpr int32_t ERR_OPNOTSUPP = -18;
constexpr int32_t ERR_NOTSOCK   = -19;

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
