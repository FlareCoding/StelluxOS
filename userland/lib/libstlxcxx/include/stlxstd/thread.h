#ifndef STLXSTD_THREAD_H
#define STLXSTD_THREAD_H

#include <stlx/proc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <new>

namespace stlxstd {
namespace detail {

struct thread_context {
    void (*invoke)(thread_context*);
};

template<typename Fn>
struct thread_context_impl : thread_context {
    Fn fn;

    explicit thread_context_impl(Fn&& f) : fn(static_cast<Fn&&>(f)) {
        invoke = [](thread_context* base) {
            auto* self = static_cast<thread_context_impl*>(base);
            self->fn();
        };
    }
};

// Defined in thread.cpp
extern "C" void stlxstd_thread_entry(void* arg);

} // namespace detail

class thread {
public:
    static constexpr size_t STACK_SIZE = 64 * 1024;

    thread() : m_handle(-1), m_stack(nullptr) {}

    template<typename Fn>
    explicit thread(Fn&& fn) {
        using ctx_t = detail::thread_context_impl<Fn>;
        auto* ctx = static_cast<ctx_t*>(malloc(sizeof(ctx_t)));
        if (!ctx) abort();
        new (ctx) ctx_t(static_cast<Fn&&>(fn));

        m_stack = mmap(nullptr, STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (m_stack == MAP_FAILED) {
            ctx->~ctx_t();
            free(ctx);
            abort();
        }

        void* stack_top = static_cast<char*>(m_stack) + STACK_SIZE;
        m_handle = proc_create_thread(detail::stlxstd_thread_entry, ctx,
                                      stack_top, "stlxstd");
        if (m_handle < 0) {
            ctx->~ctx_t();
            free(ctx);
            munmap(m_stack, STACK_SIZE);
            m_stack = nullptr;
            abort();
        }

        proc_thread_start(m_handle);
    }

    ~thread() {
        if (joinable()) abort();
    }

    thread(const thread&) = delete;
    thread& operator=(const thread&) = delete;

    thread(thread&& o) : m_handle(o.m_handle), m_stack(o.m_stack) {
        o.m_handle = -1;
        o.m_stack = nullptr;
    }

    thread& operator=(thread&& o) {
        if (this != &o) {
            if (joinable()) abort();
            m_handle = o.m_handle;
            m_stack = o.m_stack;
            o.m_handle = -1;
            o.m_stack = nullptr;
        }
        return *this;
    }

    bool joinable() const { return m_handle >= 0; }

    void join() {
        if (!joinable()) return;
        proc_thread_join(m_handle, nullptr);
        munmap(m_stack, STACK_SIZE);
        m_handle = -1;
        m_stack = nullptr;
    }

    // Stack is intentionally not freed on detach. The thread is still
    // using it. It will be reclaimed when the process exits.
    void detach() {
        if (!joinable()) return;
        proc_thread_detach(m_handle);
        m_handle = -1;
        m_stack = nullptr;
    }

private:
    int m_handle;
    void* m_stack;
};

} // namespace stlxstd

#endif // STLXSTD_THREAD_H
