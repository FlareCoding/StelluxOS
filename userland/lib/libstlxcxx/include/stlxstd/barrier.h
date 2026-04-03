#ifndef STLXSTD_BARRIER_H
#define STLXSTD_BARRIER_H

#include <stlx/barrier.h>
#include <stdint.h>

namespace stlxstd {

class barrier {
public:
    explicit barrier(uint32_t count) {
        stlx_barrier_init(&b_, count);
    }

    ~barrier() = default;

    barrier(const barrier&) = delete;
    barrier& operator=(const barrier&) = delete;

    void arrive_and_wait() { stlx_barrier_wait(&b_); }

private:
    stlx_barrier_t b_;
};

} // namespace stlxstd

#endif // STLXSTD_BARRIER_H
