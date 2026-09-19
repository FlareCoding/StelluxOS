#ifndef STELLUX_TESTS_FRAMEWORK_HELPERS_H
#define STELLUX_TESTS_FRAMEWORK_HELPERS_H

#include "common/types.h"
#include "clock/clock.h"
#include "sync/atomic.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/sched_internal.h"
#include "mm/mm.h"
#include "mm/paging.h"
#include "mm/pmm.h"

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

} // namespace test_helpers

#endif // STELLUX_TESTS_FRAMEWORK_HELPERS_H
