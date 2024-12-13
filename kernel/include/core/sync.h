#ifndef SYNC_H
#define SYNC_H
#include <types.h>

#define SPINLOCK_STATE_UNLOCKED  0
#define SPINLOCK_STATE_LOCKED    1

#ifdef ARCH_X86_64
    #define memory_barrier() asm volatile("mfence" ::: "memory");
    #define load_memory_barrier() asm volatile("lfence" ::: "memory");
    #define store_memory_barrier() asm volatile("sfence" ::: "memory");
#else
    #define memory_barrier()
    #define load_memory_barrier()
    #define store_memory_barrier()
#endif

class spinlock {
public:
    explicit spinlock() : m_state(SPINLOCK_STATE_UNLOCKED) {}

    // Acquire the lock
    void lock();

    // Release the lock
    void unlock();

    // Try to acquire the lock (non-blocking)
    bool try_lock();

private:
    volatile int m_state; // 0 = unlocked, 1 = locked

    // Atomic exchange function
    int _atomic_xchg(volatile int* addr, int new_value);
};

class spinlock_guard {
public:
    explicit spinlock_guard(spinlock& lock) : m_lock(lock) {
        m_lock.lock();
    }

    ~spinlock_guard() {
        m_lock.unlock();
    }

private:
    spinlock& m_lock;
};

#endif // SYNC_H