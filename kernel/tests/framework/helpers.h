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
#include "percpu/percpu.h"
#include "smp/smp.h"

namespace test_helpers {

// Wall-clock bound: iteration counts vary ~100x across hosts and emulators.
constexpr uint64_t SPIN_TIMEOUT_NS = 20000000000ULL; // 20s

// How long `stays_asleep` watches a task, many ticks so CPU time read up to a tick behind tells sleeping from spinning
constexpr uint64_t SLEEP_WATCH_NS = 200000000ULL; // 200ms

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

// A busy delay that reads no clock, out of line and aligned to at least its size so its loop stays within one page,
// since QEMU's x86_64 emulation runs the loop about five times slower when it crosses a page boundary
__attribute__((noinline, aligned(128))) inline void brief_delay() {
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

// Starts a kernel task with `flags` on the CPU after the caller's, held until `unpin` as `start_pinned_task`'s is.
// A test runs on its CPU's idle task, so a task spinning there would hang the run instead of failing its test
inline sched::task* start_pinned_task_on_another_cpu(
    void (*entry)(void*),
    void* arg,
    const char* name,
    uint32_t flags
) {
    sched::task* t = nullptr;
    RUN_ELEVATED({
        uint32_t cpu = (percpu::current_cpu_id() + 1) % smp::cpu_count();

        t = sched::create_kernel_task(entry, arg, name, flags);
        if (t) {
            t->add_ref();
            sched::enqueue_on(t, cpu);
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

// True when `t` is seen blocked, false when it is seen dead or `SPIN_TIMEOUT_NS` passes first
inline bool blocks_before_deadline(sched::task* t) {
    uint64_t deadline = clock::now_ns() + SPIN_TIMEOUT_NS;
    uint32_t state = t->state.load_acquire();
    while (state != sched::TASK_STATE_BLOCKED) {
        if (state == sched::TASK_STATE_DEAD || clock::now_ns() > deadline) {
            return false;
        }

        state = t->state.load_acquire();
    }

    return true;
}

// True when `t` is seen blocked and then runs for under a quarter of `SLEEP_WATCH_NS`. A task that spins instead of
// sleeping runs for nearly all of the watch
inline bool stays_asleep(sched::task* t) {
    if (!blocks_before_deadline(t)) {
        return false;
    }

    uint64_t ran_before = 0;
    RUN_ELEVATED(ran_before = sched::read_task_cpu_time_ns(t));

    uint64_t watch_start = clock::now_ns();
    while (clock::now_ns() - watch_start < SLEEP_WATCH_NS) {
        brief_delay();
    }

    uint64_t ran_after = 0;
    RUN_ELEVATED(ran_after = sched::read_task_cpu_time_ns(t));

    return ran_after - ran_before < SLEEP_WATCH_NS / 4;
}

// Waits for `done` as `spin_wait` does, and when that gives up, marks `t` for a kill, waits again and returns false.
// `t` must be pinned and must set `done` once a kill ends its wait, so that it stops using what its test frees
inline bool finish_or_kill(sched::task* t, const sync::atomic<uint32_t>& done) {
    if (spin_wait(done)) {
        return true;
    }

    RUN_ELEVATED(sched::force_wake_for_kill(t));
    (void)spin_wait(done);

    return false;
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
