#ifndef STLXSTD_THREAD_H
#define STLXSTD_THREAD_H

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <new>

namespace stlxstd {

template<typename T> struct remove_ref { using type = T; };
template<typename T> struct remove_ref<T&> { using type = T; };
template<typename T> struct remove_ref<T&&> { using type = T; };

namespace detail {

struct thread_context {
    void (*invoke)(thread_context*);
    void (*destroy)(thread_context*);
};

template<typename Fn>
struct thread_context_impl : thread_context {
    Fn fn;

    static void call(thread_context* base) {
        auto* self = static_cast<thread_context_impl*>(base);
        self->fn();
    }

    static void destruct(thread_context* base) {
        static_cast<thread_context_impl*>(base)->~thread_context_impl();
    }

    explicit thread_context_impl(Fn&& f) : fn(static_cast<Fn&&>(f)) {
        invoke = &call;
        destroy = &destruct;
    }
};

// Defined in thread.cpp
extern "C" void* stlxstd_thread_entry(void* arg);

} // namespace detail

// A POSIX thread, since the C library gives only the threads it creates their own
// thread-local storage and locks its shared state once any exist
class thread {
public:
    thread() = default;

    template<typename Fn>
    explicit thread(Fn&& fn) {
        // Decay Fn so lvalue references become value types (same as std::thread).
        // Without this, passing an lvalue stores a reference that can dangle.
        using Decayed = typename remove_ref<Fn>::type;
        using ctx_t = detail::thread_context_impl<Decayed>;
        auto* ctx = static_cast<ctx_t*>(malloc(sizeof(ctx_t)));
        if (!ctx) abort();

        new (ctx) ctx_t(static_cast<Fn&&>(fn));

        if (pthread_create(&m_native, nullptr, detail::stlxstd_thread_entry, ctx) != 0) {
            ctx->~ctx_t();
            free(ctx);
            abort();
        }

        m_joinable = true;
    }

    ~thread() {
        if (joinable()) abort();
    }

    thread(const thread&) = delete;
    thread& operator=(const thread&) = delete;

    thread(thread&& o) : m_native(o.m_native), m_joinable(o.m_joinable) {
        o.m_joinable = false;
    }

    thread& operator=(thread&& o) {
        if (this != &o) {
            if (joinable()) abort();
            m_native = o.m_native;
            m_joinable = o.m_joinable;
            o.m_joinable = false;
        }
        return *this;
    }

    bool joinable() const { return m_joinable; }

    void join() {
        if (!joinable()) return;

        if (pthread_join(m_native, nullptr) != 0) {
            abort();
        }

        m_joinable = false;
    }

    void detach() {
        if (!joinable()) return;

        if (pthread_detach(m_native) != 0) {
            abort();
        }

        m_joinable = false;
    }

private:
    pthread_t m_native{};
    bool m_joinable = false;
};

} // namespace stlxstd

#endif // STLXSTD_THREAD_H
