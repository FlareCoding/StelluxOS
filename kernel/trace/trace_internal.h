#ifndef STELLUX_TRACE_INTERNAL_H
#define STELLUX_TRACE_INTERNAL_H

#include "common/types.h"

namespace trace {

enum class phase : uint8_t {
    complete   = 'X', // duration span (one record per span)
    instant    = 'i', // point-in-time event
    counter    = 'C', // numeric sample
    flow_begin = 's',
    flow_end   = 'f',
};

enum class flags : uint8_t {
    none       = 0,
    privileged = 1 << 0, // recorded inside an elevated window
    nmi        = 1 << 1, // emitted from NMI context
};

struct record {
    uint64_t ts;
    const char* name;
    uint64_t arg;
    uint32_t tid;
    uint16_t category;
    phase    ph;
    flags    fl;
} __attribute__((packed));

static_assert(sizeof(record) == 32, "tracing record must be 32 bytes");

class trace_buffer {
public:
    static constexpr size_t CAPACITY = 32 * 1024; // 1 MiB
    static constexpr size_t MASK     = CAPACITY - 1;
    static_assert((CAPACITY & MASK) == 0, "CAPACITY must be power of two");

    inline void push(const record& rec) {
        uint64_t idx = __atomic_fetch_add(&m_head, 1, __ATOMIC_RELAXED);
        m_records[idx & MASK] = rec;
    }

    inline uint64_t head() const {
        return __atomic_load_n(&m_head, __ATOMIC_ACQUIRE);
    }

    inline void clear() {
        __atomic_store_n(&m_head, 0, __ATOMIC_RELEASE);
    }

    inline const record* slot(uint64_t logical_index) const {
        return &m_records[logical_index & MASK];
    }

private:
    alignas(64) record   m_records[CAPACITY];
    alignas(64) uint64_t m_head{0};
};

} // namespace trace

#endif // STELLUX_TRACE_INTERNAL_H
