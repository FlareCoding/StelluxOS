#include "net/tcp/timers.h"
#include "dynpriv/dynpriv.h"

namespace net {
namespace tcp {

static void release(record* rec) {
    if (rec->release()) {
        record::ref_destroy(rec);
    }
}

void arm_timer(record* rec, timer::deadline_timer* timer, uint64_t deadline_ns) {
    rec->add_ref();
    RUN_ELEVATED(timer::schedule(timer, deadline_ns));
}

void disarm_timer(record* rec, timer::deadline_timer* timer) {
    timer::cancel_outcome outcome = timer::cancel_outcome::not_scheduled;
    RUN_ELEVATED(outcome = timer::cancel(timer));

    if (outcome == timer::cancel_outcome::removed) {
        release(rec);
    }
}

void finish_timer_callback(record* rec) {
    release(rec);
}

} // namespace tcp
} // namespace net
