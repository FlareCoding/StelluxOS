#ifndef SYNC_H
#define SYNC_H
#include "ktypes.h"

// Declare a spinlock type
typedef struct {
    volatile int lockVar;
} Spinlock;

// Macro to declare a new spinlock
#define DECLARE_SPINLOCK(name) Spinlock name = { .lockVar = 0 }

// Acquire the lock
static inline void acquireSpinlock(Spinlock* lock) {
    while (__sync_lock_test_and_set(&lock->lockVar, 1)) {
        asm volatile("pause");
    }
}

// Release the lock
static inline void releaseSpinlock(Spinlock* lock) {
    __sync_lock_release(&lock->lockVar);
}

#endif
