#include <stlx/barrier.h>
#include <stlx/futex.h>

void stlx_barrier_init(stlx_barrier_t* b, uint32_t count) {
    b->count = 0;
    b->generation = 0;
    b->total = count;
}

void stlx_barrier_wait(stlx_barrier_t* b) {
    uint32_t gen = __atomic_load_n(&b->generation, __ATOMIC_ACQUIRE);

    // Count grows monotonically (no reset). The last thread in each round
    // is identified by count being a multiple of total. This eliminates
    // the race between resetting count and advancing generation.
    uint64_t arrived = __atomic_fetch_add(&b->count, 1, __ATOMIC_ACQ_REL) + 1;

    if (arrived % b->total == 0) {
        __atomic_fetch_add(&b->generation, 1, __ATOMIC_RELEASE);
        stlx_futex_wake_all(&b->generation);
    } else {
        while (__atomic_load_n(&b->generation, __ATOMIC_ACQUIRE) == gen) {
            stlx_futex_wait(&b->generation, gen, 0);
        }
    }
}
