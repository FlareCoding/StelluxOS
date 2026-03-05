#include "resource/providers/proc_provider.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "fs/node.h"
#include "common/logging.h"

namespace resource::proc_provider {

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

__PRIVILEGED_CODE int32_t terminate_proc_resource(
    proc_resource* pr,
    int32_t exit_code,
    bool wait_for_exit
) {
    if (!pr) {
        return ERR_INVAL;
    }

    sched::task* created_child = nullptr;
    sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);

    if (!pr->child || pr->exited) {
        sync::spin_unlock_irqrestore(pr->lock, irq);
        return OK;
    }

    if (pr->child->state == sched::TASK_STATE_CREATED) {
        created_child = pr->child;
        pr->exit_code = exit_code;
        pr->exited = true;
        pr->child = nullptr;
        sync::wake_all(pr->wait_queue);
        sync::spin_unlock_irqrestore(pr->lock, irq);

        if (created_child->proc_res) {
            (void)created_child->proc_res->release();
            created_child->proc_res = nullptr;
        }
        destroy_unstarted_task(created_child);
        return OK;
    }

    sched::request_terminate(pr->child, exit_code);
    if (wait_for_exit) {
        while (!pr->exited) {
            irq = sync::wait(pr->wait_queue, pr->lock, irq);
        }
    }
    sync::spin_unlock_irqrestore(pr->lock, irq);
    return OK;
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
        (void)terminate_proc_resource(pr, PROC_KILL_EXIT_CODE, true);
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
    pr->exited = false;
    pr->detached = false;

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
