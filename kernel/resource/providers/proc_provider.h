#ifndef STELLUX_RESOURCE_PROVIDERS_PROC_PROVIDER_H
#define STELLUX_RESOURCE_PROVIDERS_PROC_PROVIDER_H

#include "resource/resource.h"
#include "sync/wait_queue.h"
#include "rc/ref_counted.h"
#include "rc/strong_ref.h"

namespace resource::proc_provider {

struct proc_resource : rc::ref_counted<proc_resource> {
    sync::spinlock    lock;
    sched::task*      child;
    sync::wait_queue  wait_queue;
    int32_t           wait_status;
    bool              exited;
    bool              detached;

    /**
     * @brief Free a proc_resource when the last reference is released.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(proc_resource* self);
};

struct proc_resource_impl {
    rc::strong_ref<proc_resource> proc;
};

/**
 * @brief Create a PROCESS resource wrapping a child task.
 * Sets child_task->proc_res and gives the child an owned ref on proc_resource.
 * Returns a resource_object with refcount=1 (caller owns it).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_proc_resource(
    sched::task* child_task,
    resource_object** out_obj
);

/**
 * @brief Get the proc_resource from a PROCESS resource_object.
 * Returns nullptr if obj is not a PROCESS resource or has no impl.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE proc_resource* get_proc_resource(resource_object* obj);

/**
 * @brief Destroy a task that was created but never started (TASK_STATE_CREATED).
 * Frees mm_ctx, system stack, and the task struct. Does NOT release proc_res ref.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void destroy_unstarted_task(sched::task* t);

} // namespace resource::proc_provider

#endif // STELLUX_RESOURCE_PROVIDERS_PROC_PROVIDER_H
