#ifndef STELLUX_TESTS_FRAMEWORK_HELPERS_H
#define STELLUX_TESTS_FRAMEWORK_HELPERS_H

#include "common/types.h"
#include "clock/clock.h"
#include "exec/elf.h"
#include "resource/providers/proc_provider.h"
#include "sync/atomic.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/sched_internal.h"
#include "mm/mm.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "dynpriv/dynpriv.h"

namespace test_helpers {

// Wall-clock bound: iteration counts vary ~100x across hosts and emulators.
constexpr uint64_t SPIN_TIMEOUT_NS = 20000000000ULL; // 20s

// Runs the calling task under a user address space so copies reach it the
// way a syscall body does, then puts the task's own root back
struct user_space_scope {
    sched::task* self;
    pmm::phys_addr_t saved_root;

    explicit user_space_scope(mm::mm_context* ctx)
        : self(sched::current()), saved_root(self->exec.pt_root) {
        self->exec.mm_ctx = ctx;
        self->exec.pt_root = paging::supervisor_pt_root_for_user_task(ctx->pt_root);
        self->exec.user_pt_root = ctx->pt_root;
        sched::arch_post_switch(self);
    }

    ~user_space_scope() {
        self->exec.mm_ctx = nullptr;
        self->exec.pt_root = saved_root;
        self->exec.user_pt_root = 0;
        sched::arch_post_switch(self);
    }
};

// One eager page in a fresh user address space, reachable from the kernel through
// its frame. Syscalls that copy through it run under user_space_scope(page.ctx).
struct user_page {
    mm::mm_context* ctx = nullptr;
    uintptr_t addr = 0;
    uint8_t* bytes = nullptr;

    user_page() {
        ctx = mm::mm_context_create();
        if (!ctx) {
            return;
        }

        uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
        uint32_t flags = mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS;
        if (mm::mm_context_map_anonymous(ctx, 0, pmm::PAGE_SIZE, prot, flags, &addr) != mm::MM_CTX_OK) {
            addr = 0;
            return;
        }

        pmm::phys_addr_t phys = paging::get_physical(addr, ctx->pt_root);
        bytes = phys ? static_cast<uint8_t*>(paging::phys_to_virt(phys)) : nullptr;
    }

    ~user_page() {
        if (ctx) {
            mm::mm_context_release(ctx);
        }
    }

    bool ready() const { return bytes != nullptr; }

    template <typename T>
    T* at(size_t offset) { return reinterpret_cast<T*>(bytes + offset); }
};

inline bool spin_wait(const sync::atomic<uint32_t>& flag) {
    uint64_t deadline = clock::now_ns() + SPIN_TIMEOUT_NS;
    while (!flag.load_acquire()) {
        if (clock::now_ns() > deadline) return false;
    }
    return true;
}

inline bool spin_wait_ge(const sync::atomic<uint32_t>& value, uint32_t target) {
    uint64_t deadline = clock::now_ns() + SPIN_TIMEOUT_NS;
    while (value.load_acquire() < target) {
        if (clock::now_ns() > deadline) return false;
    }
    return true;
}

inline void brief_delay() {
    uint64_t i = 0;
    while (i < 5000000) {
        asm volatile("" : "+r"(i));
        i++;
    }
}

// Starts an elevated kernel task pinned by a reference, so its state can be watched until unpin
inline sched::task* start_pinned_task(void (*entry)(void*), void* arg, const char* name) {
    sched::task* t = nullptr;
    RUN_ELEVATED({
        t = sched::create_kernel_task(entry, arg, name, sched::TASK_FLAG_ELEVATED);
        if (t) {
            t->add_ref();
            sched::enqueue(t);
        }
    });

    return t;
}

inline void unpin(sched::task* t) {
    RUN_ELEVATED({
        if (t->release()) {
            sched::task::ref_destroy(t);
        }
    });
}

inline bool blocks_before_deadline(sched::task* t) {
    uint64_t deadline = clock::now_ns() + SPIN_TIMEOUT_NS;
    while (t->state.load_acquire() != sched::TASK_STATE_BLOCKED) {
        if (clock::now_ns() > deadline) {
            return false;
        }
    }

    return true;
}

// A process loaded from the program at `path` and not yet started, or null when it cannot be loaded
inline sched::task* create_unstarted_process(const char* path) {
    exec::loaded_image loaded;
    if (exec::load_elf(path, &loaded) != exec::OK) {
        return nullptr;
    }

    return sched::create_user_task(&loaded, path);
}

// The handle object of such a process, owned by the caller, or null
inline resource::resource_object* create_unstarted_process_object(const char* path) {
    sched::task* process = create_unstarted_process(path);
    if (!process) {
        return nullptr;
    }

    resource::resource_object* obj = nullptr;
    if (resource::proc_provider::create_proc_resource(process, &obj) != resource::OK) {
        resource::proc_provider::destroy_unstarted_task(process);
        return nullptr;
    }

    return obj;
}

} // namespace test_helpers

#endif // STELLUX_TESTS_FRAMEWORK_HELPERS_H
