#ifndef STLXSTD_MUTEX_H
#define STLXSTD_MUTEX_H

#include <stlx/mutex.h>

namespace stlxstd {

struct defer_lock_t {};
inline constexpr defer_lock_t defer_lock{};

class mutex {
public:
    mutex() : m_native(STLX_MUTEX_INIT) {}
    ~mutex() = default;

    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;

    void lock() { stlx_mutex_lock(&m_native); }
    void unlock() { stlx_mutex_unlock(&m_native); }
    bool try_lock() { return stlx_mutex_trylock(&m_native) == 0; }

    stlx_mutex_t* native() { return &m_native; }

private:
    stlx_mutex_t m_native;
};

template<typename Mutex>
class lock_guard {
public:
    explicit lock_guard(Mutex& m) : m_mutex(m) { m_mutex.lock(); }
    ~lock_guard() { m_mutex.unlock(); }

    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

private:
    Mutex& m_mutex;
};

template<typename Mutex>
class unique_lock {
public:
    explicit unique_lock(Mutex& m) : m_mutex(&m), m_owned(true) { m_mutex->lock(); }
    unique_lock(Mutex& m, defer_lock_t) : m_mutex(&m), m_owned(false) {}
    ~unique_lock() { if (m_owned) m_mutex->unlock(); }

    unique_lock(const unique_lock&) = delete;
    unique_lock& operator=(const unique_lock&) = delete;

    unique_lock(unique_lock&& o) : m_mutex(o.m_mutex), m_owned(o.m_owned) {
        o.m_mutex = nullptr;
        o.m_owned = false;
    }

    unique_lock& operator=(unique_lock&& o) {
        if (this != &o) {
            if (m_owned) m_mutex->unlock();
            m_mutex = o.m_mutex;
            m_owned = o.m_owned;
            o.m_mutex = nullptr;
            o.m_owned = false;
        }
        return *this;
    }

    void lock() { m_mutex->lock(); m_owned = true; }
    void unlock() { m_mutex->unlock(); m_owned = false; }
    bool try_lock() { m_owned = m_mutex->try_lock(); return m_owned; }

    bool owns_lock() const { return m_owned; }
    Mutex* mutex_ptr() const { return m_mutex; }

private:
    Mutex* m_mutex;
    bool m_owned;
};

} // namespace stlxstd

#endif // STLXSTD_MUTEX_H
