#include "resource/providers/proc_provider.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "percpu/percpu.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "fs/node.h"
#include "common/logging.h"

namespace resource::proc_provider {

static uint32_t g_next_terminate_epoch = 1;
static DEFINE_PER_CPU(uint32_t, proc_terminate_epoch);
static DEFINE_PER_CPU(uint32_t, proc_terminate_depth);

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

__PRIVILEGED_CODE static uint32_t acquire_terminate_epoch() {
    uint32_t depth = this_cpu(proc_terminate_depth);
    if (depth == 0) {
        uint32_t epoch = __atomic_fetch_add(&g_next_terminate_epoch, 1, __ATOMIC_ACQ_REL);
        if (epoch == 0) {
            epoch = __atomic_fetch_add(&g_next_terminate_epoch, 1, __ATOMIC_ACQ_REL);
        }
        this_cpu(proc_terminate_epoch) = epoch;
    }
    this_cpu(proc_terminate_depth) = depth + 1;
    return this_cpu(proc_terminate_epoch);
}

__PRIVILEGED_CODE static void release_terminate_epoch() {
    uint32_t depth = this_cpu(proc_terminate_depth);
    if (depth == 0) {
        return;
    }

    depth--;
    this_cpu(proc_terminate_depth) = depth;
    if (depth == 0) {
        this_cpu(proc_terminate_epoch) = 0;
    }
}

__PRIVILEGED_CODE static uint32_t collect_process_handles(
    sched::task* task,
    resource_object** out,
    uint32_t cap
) {
    if (!task || !out || cap == 0) {
        return 0;
    }

    uint32_t count = 0;
    sync::irq_state irq = sync::spin_lock_irqsave(task->handles.lock);
    for (uint32_t i = 0; i < resource::MAX_TASK_HANDLES && count < cap; i++) {
        const resource::handle_entry& entry = task->handles.entries[i];
        if (!entry.used || entry.type != resource::resource_type::PROCESS || !entry.obj) {
            continue;
        }

        resource::resource_add_ref(entry.obj);
        out[count++] = entry.obj;
    }
    sync::spin_unlock_irqrestore(task->handles.lock, irq);
    return count;
}

__PRIVILEGED_CODE static int32_t terminate_proc_resource_with_epoch(
    proc_resource* pr,
    int32_t exit_code,
    bool wait_for_exit,
    uint32_t epoch
) {
    if (!pr) {
        return ERR_INVAL;
    }

    sched::task* created_child = nullptr;
    sched::task* target_child = nullptr;
    resource_object* descendants[resource::MAX_TASK_HANDLES];
    uint32_t descendant_count = 0;
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

        if (wait_for_exit) {
            while (!pr->exited) {
                irq = sync::wait(pr->wait_queue, pr->lock, irq);
            }
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
    descendant_count = collect_process_handles(
        target_child, descendants, resource::MAX_TASK_HANDLES);
    sync::spin_unlock_irqrestore(pr->lock, irq);

    for (uint32_t i = 0; i < descendant_count; i++) {
        proc_resource* child_pr = get_proc_resource(descendants[i]);
        if (child_pr) {
            (void)terminate_proc_resource_with_epoch(child_pr, exit_code, true, epoch);
        }
        resource::resource_release(descendants[i]);
    }

    irq = sync::spin_lock_irqsave(pr->lock);
    if (!pr->exited && pr->child) {
        sched::request_terminate(pr->child, exit_code);
    }

    if (wait_for_exit) {
        while (!pr->exited) {
            irq = sync::wait(pr->wait_queue, pr->lock, irq);
        }
        pr->terminate_in_progress = false;
        sync::spin_unlock_irqrestore(pr->lock, irq);
        return OK;
    }
    sync::spin_unlock_irqrestore(pr->lock, irq);
    return OK;
}

__PRIVILEGED_CODE int32_t terminate_proc_resource(
    proc_resource* pr,
    int32_t exit_code,
    bool wait_for_exit
) {
    uint32_t epoch = acquire_terminate_epoch();
    int32_t rc = terminate_proc_resource_with_epoch(pr, exit_code, wait_for_exit, epoch);
    release_terminate_epoch();
    return rc;
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
