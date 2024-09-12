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

    // Memory barrier to prevent reordering
    __sync_synchronize();
}

// Release the lock
static inline void releaseSpinlock(Spinlock* lock) {
    // Memory barrier to prevent reordering
    __sync_synchronize();
    __sync_lock_release(&lock->lockVar);
}

// Function to check if the spinlock is currently locked
static inline int isSpinlockLocked(Spinlock* lock) {
    // Just read the lockVar without modifying it
    return __sync_val_compare_and_swap(&lock->lockVar, 1, 1);
}

#endif
