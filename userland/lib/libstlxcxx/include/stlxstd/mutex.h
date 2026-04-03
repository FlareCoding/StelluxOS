#ifndef STLXSTD_MUTEX_H
#define STLXSTD_MUTEX_H

#include <stlx/mutex.h>

namespace stlxstd {

struct defer_lock_t {};
inline constexpr defer_lock_t defer_lock{};

class mutex {
public:
    mutex() : m_(STLX_MUTEX_INIT) {}
    ~mutex() = default;

    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;

    void lock() { stlx_mutex_lock(&m_); }
    void unlock() { stlx_mutex_unlock(&m_); }
    bool try_lock() { return stlx_mutex_trylock(&m_) == 0; }

    stlx_mutex_t* native() { return &m_; }

private:
    stlx_mutex_t m_;
};

template<typename Mutex>
class lock_guard {
public:
    explicit lock_guard(Mutex& m) : m_(m) { m_.lock(); }
    ~lock_guard() { m_.unlock(); }

    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

private:
    Mutex& m_;
};

template<typename Mutex>
class unique_lock {
public:
    explicit unique_lock(Mutex& m) : m_(&m), owned_(true) { m_->lock(); }
    unique_lock(Mutex& m, defer_lock_t) : m_(&m), owned_(false) {}
    ~unique_lock() { if (owned_) m_->unlock(); }

    unique_lock(const unique_lock&) = delete;
    unique_lock& operator=(const unique_lock&) = delete;

    unique_lock(unique_lock&& o) : m_(o.m_), owned_(o.owned_) {
        o.m_ = nullptr;
        o.owned_ = false;
    }

    unique_lock& operator=(unique_lock&& o) {
        if (this != &o) {
            if (owned_) m_->unlock();
            m_ = o.m_;
            owned_ = o.owned_;
            o.m_ = nullptr;
            o.owned_ = false;
        }
        return *this;
    }

    void lock() { m_->lock(); owned_ = true; }
    void unlock() { m_->unlock(); owned_ = false; }
    bool try_lock() { owned_ = m_->try_lock(); return owned_; }

    bool owns_lock() const { return owned_; }
    Mutex* mutex_ptr() const { return m_; }

private:
    Mutex* m_;
    bool owned_;
};

} // namespace stlxstd

#endif // STLXSTD_MUTEX_H
