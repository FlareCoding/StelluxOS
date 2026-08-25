#ifndef STLXSTD_BARRIER_H
#define STLXSTD_BARRIER_H

#include <stlx/barrier.h>
#include <stdint.h>

namespace stlxstd {

class barrier {
public:
    explicit barrier(uint32_t count) {
        stlx_barrier_init(&m_barrier, count);
    }

    ~barrier() = default;

    barrier(const barrier&) = delete;
    barrier& operator=(const barrier&) = delete;

    void arrive_and_wait() { stlx_barrier_wait(&m_barrier); }

private:
    stlx_barrier_t m_barrier;
};

} // namespace stlxstd

#endif // STLXSTD_BARRIER_H
