#include <sync.h>
#include <serial/serial.h>

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
