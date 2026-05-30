#ifndef STELLUX_TRACE_H
#define STELLUX_TRACE_H

#include "trace_internal.h"
#include "trace_categories.h"

namespace trace {

constexpr int32_t OK            =  0;
constexpr int32_t ERR_NO_MEM    = -1;
constexpr int32_t ERR_NOT_READY = -2;
constexpr int32_t ERR_IO        = -3;
constexpr int32_t ERR_BUSY      = -4;

constexpr uint32_t KTRACE_IOCTL_SET_CATEGORIES = 0x4b01;
constexpr uint32_t KTRACE_IOCTL_RESET          = 0x4b02;

int32_t     init(); // must be called per-cpu
void        set_enabled_categories(category mask);
uint16_t    enabled_categories();

int32_t     begin_dump();    // pause capture for a consistent snapshot, ERR_BUSY if active
void        end_dump();      // resume capture after a dump completes
void        reset_buffers(); // clear all per-CPU ring buffers

size_t      dump_read(uint64_t offset, void* buf, size_t count);
uint64_t    dump_size();

/*
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_device();

void        emit_record(const record& rec);
void        emit_event(category cat, const char* name, phase ph,
                       uint32_t tid, uint32_t pid,
                       uint64_t arg0, uint64_t arg1, flags fl = flags::none);

class scope {
public:
    scope(uint16_t category, const char* name);
    ~scope();

    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;

private:
    uint64_t    m_start_ts;
    uint16_t    m_category;
    const char* m_name;
};

} // namespace trace

#define STLX_TRACE_CAT_(a, b) a##b
#define STLX_TRACE_CAT(a, b)  STLX_TRACE_CAT_(a, b)

#ifdef STLX_TRACING_ENABLED

#define TRACE_SCOPE(cat, name) \
    ::trace::scope STLX_TRACE_CAT(_trace_scope_, __LINE__){cat, name}

#define TRACE_EVENT(cat, name, tid, pid, a0, a1, fl) \
    ::trace::emit_event((cat), (name), ::trace::phase::instant, (tid), (pid), (a0), (a1), (fl))

#define TRACE_INSTANT(cat, name) \
    ::trace::emit_event((cat), (name), ::trace::phase::instant, \
        ::sched::current() ? ::sched::current()->tid : 0, \
        ::sched::current() ? ::sched::process_id(::sched::current()) : 0, 0, 0)

#else

#define TRACE_SCOPE(cat, name) ((void)0)
#define TRACE_EVENT(cat, name, tid, pid, a0, a1, fl) ((void)0)
#define TRACE_INSTANT(cat, name) ((void)0)

#endif // STLX_TRACING_ENABLED

#endif // STELLUX_TRACE_H
