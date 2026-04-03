#include <stlx/mutex.h>
#include <stlx/futex.h>

void stlx_mutex_lock(stlx_mutex_t* m) {
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&m->state, &c, 1, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return;
    }

    do {
        if (c == 2 || __atomic_compare_exchange_n(&m->state, &c, 2, 0,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            stlx_futex_wait(&m->state, 2, 0);
        }
        c = 0;
    } while (!__atomic_compare_exchange_n(&m->state, &c, 2, 0,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
}

void stlx_mutex_unlock(stlx_mutex_t* m) {
    uint32_t prev = __atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE);
    if (prev == 2) {
        stlx_futex_wake(&m->state, 1);
    }
}

int stlx_mutex_trylock(stlx_mutex_t* m) {
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&m->state, &c, 1, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return 0;
    }
    return -1;
}
