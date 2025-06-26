#include <stlibc/input/input.h>
#include <stlibc/stellux_syscalls.h>

long stlx_read_input_events(uint32_t queue_id, int blocking, input_event_t* events, size_t max_events) {
    // Validate parameters
    if (!events || max_events == 0) {
        return -EINVAL;
    }
    
    // Call the syscall with the appropriate parameters
    return syscall4(SYS_READ_INPUT_EVENT, 
                   queue_id,            // arg1: queue_id
                   blocking,            // arg2: blocking flag
                   (uint64_t)events,    // arg3: events buffer
                   max_events);         // arg4: max_events
}
