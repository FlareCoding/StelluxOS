#ifndef STELLUX_DEBUG_STACKTRACE_H
#define STELLUX_DEBUG_STACKTRACE_H

#include "common/types.h"

namespace stacktrace {

struct frame {
    uint64_t return_addr;
    uint64_t frame_ptr;
};

static constexpr int MAX_FRAMES = 32;

int walk(uint64_t initial_fp, frame* out, int max_frames);

} // namespace stacktrace

#endif // STELLUX_DEBUG_STACKTRACE_H
