#ifndef SYNC_H
#define SYNC_H
#include "ktypes.h"

#define ATOMIC_MEMORY_ORDER_RELAXED __ATOMIC_RELAXED
#define ATOMIC_MEMORY_ORDER_ACQUIRE __ATOMIC_ACQUIRE
#define ATOMIC_MEMORY_ORDER_RELEASE __ATOMIC_RELEASE
#define ATOMIC_MEMORY_ORDER_ACQ_REL __ATOMIC_ACQ_REL
#define ATOMIC_MEMORY_ORDER_SEQ_CST __ATOMIC_SEQ_CST

template <typename T>
class Atomic {
public:
    Atomic(T initialValue = 0) : m_value(initialValue) {}

    T load(int memoryOrder = ATOMIC_MEMORY_ORDER_SEQ_CST) const {
        return __atomic_load_n(&m_value, memoryOrder);
    }

    void store(T val, int memoryOrder = ATOMIC_MEMORY_ORDER_SEQ_CST) {
        __atomic_store_n(&m_value, val, memoryOrder);
    }

private:
    T m_value;
};

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
