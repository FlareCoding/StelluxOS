#ifndef SYNC_H
#define SYNC_H
#include <types.h>

#define SPINLOCK_STATE_UNLOCKED  0
#define SPINLOCK_STATE_LOCKED    1

#define MUTEX_STATE_UNLOCKED     0
#define MUTEX_STATE_LOCKED       1

#ifdef ARCH_X86_64
    #define memory_barrier() asm volatile("mfence" ::: "memory");
    #define load_memory_barrier() asm volatile("lfence" ::: "memory");
    #define store_memory_barrier() asm volatile("sfence" ::: "memory");
#else
    #define memory_barrier()
    #define load_memory_barrier()
    #define store_memory_barrier()
#endif

/**
 * @class spinlock
 * @brief Implements a simple spinlock for low-level mutual exclusion.
 * 
 * A spinlock is a busy-wait synchronization primitive designed for situations where locks are held
 * for short durations. It uses atomic operations to acquire and release the lock.
 */
class spinlock {
public:
    /**
     * @brief Constructs a spinlock in the unlocked state.
     */
    explicit spinlock() : m_state(SPINLOCK_STATE_UNLOCKED) {}

    /**
     * @brief Acquires the lock.
     * 
     * Blocks execution until the lock is successfully acquired. The thread spins in a busy-wait loop.
     */
    void lock();

    /**
     * @brief Releases the lock.
     * 
     * Marks the lock as unlocked, allowing other threads to acquire it.
     */
    void unlock();

    /**
     * @brief Attempts to acquire the lock without blocking.
     * @return True if the lock was successfully acquired, false otherwise.
     * 
     * Useful for scenarios where a thread cannot block but needs to try acquiring the lock.
     */
    bool try_lock();

private:
    volatile int m_state; /** Lock state: 0 = unlocked, 1 = locked */

    /**
     * @brief Atomically exchanges the value at a given address.
     * @param addr Pointer to the value to modify.
     * @param new_value The new value to store at the address.
     * @return The old value at the address.
     * 
     * Used internally for lock operations.
     */
    int _atomic_xchg(volatile int* addr, int new_value);
};

/**
 * @class spinlock_guard
 * @brief Provides RAII-style management of a spinlock.
 * 
 * Acquires the spinlock on construction and releases it on destruction.
 */
class spinlock_guard {
public:
    /**
     * @brief Constructs a spinlock_guard and acquires the given spinlock.
     * @param lock Reference to the spinlock to manage.
     */
    explicit spinlock_guard(spinlock& lock) : m_lock(lock) {
        m_lock.lock();
    }

    /**
     * @brief Destroys the spinlock_guard and releases the managed spinlock.
     */
    ~spinlock_guard() {
        m_lock.unlock();
    }

private:
    spinlock& m_lock; /** Reference to the managed spinlock */
};

/**
 * @class mutex
 * @brief Implements a mutex for blocking mutual exclusion.
 * 
 * A mutex is a synchronization primitive that blocks the thread until the lock becomes available.
 * It uses atomic operations to ensure exclusive access.
 */
class mutex {
public:
    /**
     * @brief Constructs a mutex in the unlocked state.
     */
    explicit mutex() : m_state(MUTEX_STATE_UNLOCKED) {}

    /**
     * @brief Acquires the lock, blocking until it becomes available.
     */
    void lock();

    /**
     * @brief Releases the lock.
     * 
     * Marks the lock as unlocked, allowing other threads to acquire it.
     */
    void unlock();

    /**
     * @brief Attempts to acquire the lock without blocking.
     * @return True if the lock was successfully acquired, false otherwise.
     * 
     * Useful for scenarios where a thread cannot block but needs to try acquiring the lock.
     */
    bool try_lock();

private:
    volatile int m_state; /** Lock state: 0 = unlocked, 1 = locked */

    /**
     * @brief Atomically compares and exchanges a value at a given address.
     * @param addr Pointer to the value to modify.
     * @param expected The expected value at the address.
     * @param new_value The new value to store if the expected value matches.
     * @return True if the exchange was successful, false otherwise.
     * 
     * Used internally for lock operations.
     */
    bool _atomic_cmpxchg(volatile int* addr, int expected, int new_value);
};

/**
 * @class mutex_guard
 * @brief Provides RAII-style management of a mutex.
 * 
 * Acquires the mutex on construction and releases it on destruction.
 */
class mutex_guard {
public:
    /**
     * @brief Constructs a mutex_guard and acquires the given mutex.
     * @param mtx Reference to the mutex to manage.
     */
    explicit mutex_guard(mutex& mtx) : m_mutex(mtx) {
        m_mutex.lock();
    }

    /**
     * @brief Destroys the mutex_guard and releases the managed mutex.
     */
    ~mutex_guard() {
        m_mutex.unlock();
    }

private:
    mutex& m_mutex; /** Reference to the managed mutex */
};

#endif // SYNC_H