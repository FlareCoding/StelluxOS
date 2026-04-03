#include <stlx/barrier.h>
#include <stlx/futex.h>

void stlx_barrier_init(stlx_barrier_t* b, uint32_t count) {
    b->count = 0;
    b->generation = 0;
    b->total = count;
}

void stlx_barrier_wait(stlx_barrier_t* b) {
    uint32_t gen = __atomic_load_n(&b->generation, __ATOMIC_ACQUIRE);
    uint32_t arrived = __atomic_fetch_add(&b->count, 1, __ATOMIC_ACQ_REL) + 1;

    if (arrived == b->total) {
        __atomic_store_n(&b->count, 0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&b->generation, 1, __ATOMIC_RELEASE);
        stlx_futex_wake_all(&b->generation);
    } else {
        while (__atomic_load_n(&b->generation, __ATOMIC_ACQUIRE) == gen) {
            stlx_futex_wait(&b->generation, gen, 0);
        }
    }
}
