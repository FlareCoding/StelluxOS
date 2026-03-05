#include "resource/providers/proc_provider.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "fs/node.h"
#include "common/logging.h"

namespace resource::proc_provider {

static uint32_t g_next_terminate_epoch = 1;

__PRIVILEGED_CODE void proc_resource::ref_destroy(proc_resource* self) {
    heap::kfree_delete(self);
}

__PRIVILEGED_CODE static ssize_t proc_read(
    resource_object* obj, void* kdst, size_t count, uint32_t flags
) {
    (void)obj; (void)kdst; (void)count; (void)flags;
    return ERR_UNSUP;
}

__PRIVILEGED_CODE static ssize_t proc_write(
    resource_object* obj, const void* ksrc, size_t count, uint32_t flags
) {
    (void)obj; (void)ksrc; (void)count; (void)flags;
    return ERR_UNSUP;
}

__PRIVILEGED_CODE static uint32_t allocate_terminate_epoch() {
    uint32_t epoch = __atomic_fetch_add(&g_next_terminate_epoch, 1, __ATOMIC_ACQ_REL);
    if (epoch == 0) {
        epoch = __atomic_fetch_add(&g_next_terminate_epoch, 1, __ATOMIC_ACQ_REL);
    }
    return epoch;
}

__PRIVILEGED_CODE static resource_object* acquire_process_handle_at(
    sched::task* task,
    uint32_t index
) {
    if (!task || index >= resource::MAX_TASK_HANDLES) {
        return nullptr;
    }

    resource_object* obj = nullptr;
    sync::irq_state irq = sync::spin_lock_irqsave(task->handles.lock);
    const resource::handle_entry& entry = task->handles.entries[index];
    if (entry.used &&
        entry.type == resource::resource_type::PROCESS &&
        entry.obj) {
        resource::resource_add_ref(entry.obj);
        obj = entry.obj;
    }
    sync::spin_unlock_irqrestore(task->handles.lock, irq);
    return obj;
}

__PRIVILEGED_CODE static int32_t terminate_proc_resource_with_epoch(
    proc_resource* pr,
    int32_t exit_code,
    uint32_t epoch
) {
    if (!pr) {
        return ERR_INVAL;
    }

    sched::task* created_child = nullptr;
    sched::task* target_child = nullptr;
    sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);

    if (!pr->child || pr->exited) {
        pr->terminate_in_progress = false;
        sync::spin_unlock_irqrestore(pr->lock, irq);
        return OK;
    }

    if (pr->terminate_in_progress) {
        if (pr->terminate_epoch == epoch) {
            // Cycle detected in the same recursive termination traversal.
            // Avoid waiting here to prevent deadlock (A->B->A style graphs).
            sync::spin_unlock_irqrestore(pr->lock, irq);
            return OK;
        }

        while (!pr->exited) {
            irq = sync::wait(pr->wait_queue, pr->lock, irq);
        }
        sync::spin_unlock_irqrestore(pr->lock, irq);
        return OK;
    }

    pr->terminate_in_progress = true;
    pr->terminate_epoch = epoch;

    if (pr->child->state == sched::TASK_STATE_CREATED) {
        created_child = pr->child;
        pr->exit_code = exit_code;
        pr->exited = true;
        pr->child = nullptr;
        pr->terminate_in_progress = false;
        sync::wake_all(pr->wait_queue);
        sync::spin_unlock_irqrestore(pr->lock, irq);

        if (created_child->proc_res) {
            (void)created_child->proc_res->release();
            created_child->proc_res = nullptr;
        }
        destroy_unstarted_task(created_child);
        return OK;
    }

    target_child = pr->child;
    sync::spin_unlock_irqrestore(pr->lock, irq);

    for (uint32_t i = 0; i < resource::MAX_TASK_HANDLES; i++) {
        resource_object* descendant_obj = acquire_process_handle_at(target_child, i);
        if (!descendant_obj) {
            continue;
        }

        proc_resource* child_pr = get_proc_resource(descendant_obj);
        if (child_pr) {
            (void)terminate_proc_resource_with_epoch(child_pr, exit_code, epoch);
        }
        resource::resource_release(descendant_obj);
    }

    irq = sync::spin_lock_irqsave(pr->lock);
    if (!pr->exited && pr->child) {
        sched::request_terminate(pr->child, exit_code);
    }

    while (!pr->exited) {
        irq = sync::wait(pr->wait_queue, pr->lock, irq);
    }
    pr->terminate_in_progress = false;
    sync::spin_unlock_irqrestore(pr->lock, irq);
    return OK;
}

__PRIVILEGED_CODE int32_t terminate_proc_resource(
    proc_resource* pr,
    int32_t exit_code
) {
    uint32_t epoch = allocate_terminate_epoch();
    return terminate_proc_resource_with_epoch(pr, exit_code, epoch);
}

__PRIVILEGED_CODE static void proc_close(resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    auto* impl = static_cast<proc_resource_impl*>(obj->impl);
    auto* pr = impl->proc.ptr();

    bool should_terminate = false;
    sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);
    if (pr->child && pr->child->state == sched::TASK_STATE_CREATED) {
        should_terminate = true;
    } else if (pr->child && !pr->exited && !pr->detached) {
        should_terminate = true;
    }
    sync::spin_unlock_irqrestore(pr->lock, irq);

    if (should_terminate) {
        (void)terminate_proc_resource(pr, PROC_KILL_EXIT_CODE);
    }

    heap::kfree_delete(impl);
    obj->impl = nullptr;
}

static const resource_ops g_proc_ops = {
    proc_read,
    proc_write,
    proc_close,
    nullptr,
};

__PRIVILEGED_CODE int32_t create_proc_resource(
    sched::task* child_task,
    resource_object** out_obj
) {
    if (!child_task || !out_obj) {
        return ERR_INVAL;
    }

    auto* pr = heap::kalloc_new<proc_resource>();
    if (!pr) {
        return ERR_NOMEM;
    }

    pr->lock = sync::SPINLOCK_INIT;
    pr->child = child_task;
    pr->wait_queue.init();
    pr->exit_code = 0;
    pr->terminate_epoch = 0;
    pr->exited = false;
    pr->detached = false;
    pr->terminate_in_progress = false;

    pr->add_ref(); // refcount 1 -> 2 (second ref for the child task)

    auto* impl = heap::kalloc_new<proc_resource_impl>();
    if (!impl) {
        (void)pr->release(); // 2 -> 1
        if (pr->release()) { // 1 -> 0
            proc_resource::ref_destroy(pr);
        }
        return ERR_NOMEM;
    }
    impl->proc = rc::strong_ref<proc_resource>::adopt(pr); // takes ownership of ref 1

    auto* obj = heap::kalloc_new<resource_object>();
    if (!obj) {
        heap::kfree_delete(impl); // drops strong_ref: 2 -> 1
        if (pr->release()) { // child ref: 1 -> 0
            proc_resource::ref_destroy(pr);
        }
        return ERR_NOMEM;
    }

    obj->type = resource_type::PROCESS;
    obj->ops = &g_proc_ops;
    obj->impl = impl;

    child_task->proc_res = pr; // raw pointer, represents ref 2

    *out_obj = obj;
    return OK;
}

__PRIVILEGED_CODE proc_resource* get_proc_resource(resource_object* obj) {
    if (!obj || obj->type != resource_type::PROCESS || !obj->impl) {
        return nullptr;
    }
    auto* impl = static_cast<proc_resource_impl*>(obj->impl);
    return impl->proc.ptr();
}

__PRIVILEGED_CODE void destroy_unstarted_task(sched::task* t) {
    resource::close_all(t);
    if (t->cwd) {
        if (t->cwd->release()) {
            fs::node::ref_destroy(t->cwd);
        }
        t->cwd = nullptr;
    }

    if (t->exec.mm_ctx) {
        mm::mm_context_release(t->exec.mm_ctx);
        t->exec.mm_ctx = nullptr;
    }

    vmm::free(t->sys_stack_base);
    heap::kfree_delete(t);
}

} // namespace resource::proc_provider
