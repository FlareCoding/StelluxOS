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
    idle       = 1 << 2, // sched:switch where next is the idle task
};

struct record {
    uint64_t    ts;
    const char* name;     // interned to a u32 id on the wire
    uint64_t    arg0;     // span: duration | switch: next_tid | wakeup: waker_tid
    uint64_t    arg1;     // switch: prev_state | else 0
    uint32_t    tid;      // span/wakeup subject | switch: prev tid
    uint32_t    pid;      // process (group-leader tid; 0 = kernel)
    uint16_t    category;
    phase       ph;
    flags       fl;
    uint32_t    _pad;
} __attribute__((packed));

static_assert(sizeof(record) == 48, "tracing record must be 48 bytes");

class trace_buffer {
public:
    static constexpr size_t CAPACITY = 64 * 1024; // 3 MiB at 48B/record
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
