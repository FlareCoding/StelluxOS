#include <sync.h>
#include <serial/serial.h>
#include <process/process.h>

int spinlock::_atomic_xchg(volatile int* addr, int new_value) {
    int old_value;
    asm volatile (
        "xchg %0, %1"
        : "=r"(old_value), "+m"(*addr)
        : "0"(new_value)
        : "memory"
    );
    return old_value;
}

void spinlock::lock() {
    while (_atomic_xchg(&m_state, SPINLOCK_STATE_LOCKED) != 0) {
        asm volatile ("pause");
    }

    memory_barrier();
}

void spinlock::unlock() {
    memory_barrier();
    m_state = SPINLOCK_STATE_UNLOCKED;
}

bool spinlock::try_lock() {
    return _atomic_xchg(&m_state, SPINLOCK_STATE_LOCKED) == 0;
}

bool mutex::_atomic_cmpxchg(volatile int* addr, int expected, int new_value) {
    int prev;
    asm volatile (
        "lock cmpxchg %2, %1"
        : "=a"(prev), "+m"(*addr)
        : "r"(new_value), "0"(expected)
        : "memory"
    );
    return prev == expected;
}

void mutex::lock() {
    while (true) {
        // Attempt to acquire the lock
        if (_atomic_cmpxchg(&m_state, MUTEX_STATE_UNLOCKED, MUTEX_STATE_LOCKED)) {
            memory_barrier(); // Synchronize memory after acquiring the lock
            return;
        }

        // Yield CPU if lock is not acquired
        sched::yield();
    }
}

void mutex::unlock() {
    memory_barrier(); // Synchronize memory before releasing the lock
    m_state = MUTEX_STATE_UNLOCKED;
}

bool mutex::try_lock() {
    // Attempt to acquire the lock without blocking
    if (_atomic_cmpxchg(&m_state, MUTEX_STATE_UNLOCKED, MUTEX_STATE_LOCKED)) {
        memory_barrier();
        return true;
    }
    return false;
}
