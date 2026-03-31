#include "resource/providers/proc_provider.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "fs/node.h"
#include "common/logging.h"
#include "sync/poll.h"
#include "sync/wait_queue.h"

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

__PRIVILEGED_CODE static void proc_close(resource_object* obj) {
    if (!obj || !obj->impl) {
        return;
    }

    auto* impl = static_cast<proc_resource_impl*>(obj->impl);
    auto* pr = impl->proc.ptr();

    sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);

    if (pr->child && pr->child->state == sched::TASK_STATE_CREATED) {
        auto* child = pr->child;
        pr->child = nullptr;
        sync::spin_unlock_irqrestore(pr->lock, irq);

        if (child->proc_res) {
            (void)child->proc_res->release();
            child->proc_res = nullptr;
        }
        destroy_unstarted_task(child);
    } else if (pr->child && !pr->exited && !pr->detached) {
        sched::task* child = pr->child;
        sync::spin_unlock_irqrestore(pr->lock, irq);
        sched::force_wake_for_kill(child);
    } else {
        sync::spin_unlock_irqrestore(pr->lock, irq);
    }

    heap::kfree_delete(impl);
    obj->impl = nullptr;
}

__PRIVILEGED_CODE static uint32_t proc_poll(
    resource::resource_object* obj, sync::poll_table* pt
) {
    if (!obj || !obj->impl) return sync::POLL_NVAL;
    auto* impl = static_cast<proc_resource_impl*>(obj->impl);
    auto* pr = impl->proc.ptr();
    if (!pr) return sync::POLL_HUP;

    if (pt) {
        sync::poll_subscribe(*pt, pr->wait_queue);
    }

    sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);
    uint32_t mask = pr->exited ? sync::POLL_IN : 0;
    sync::spin_unlock_irqrestore(pr->lock, irq);
    return mask;
}

static const resource_ops g_proc_ops = {
    proc_read,
    proc_write,
    proc_close,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    proc_poll,
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
    pr->wait_status = 0;
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

    if (t->group) {
        if (t->group->leader != t && t->group_link.is_linked()) {
            sync::irq_state irq = sync::spin_lock_irqsave(t->group->lock);
            t->group->threads.remove(t);
            t->group->thread_count--;
            sync::spin_unlock_irqrestore(t->group->lock, irq);
        }
        if (t->group->release()) {
            sched::thread_group::ref_destroy(t->group);
        }
        t->group = nullptr;
    }

    vmm::free(t->sys_stack_base);
    heap::kfree_delete(t);
}

} // namespace resource::proc_provider
