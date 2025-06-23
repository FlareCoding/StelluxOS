#include <syscall/handlers/sys_time.h>
#include <time/time.h>
#include <core/klog.h>
#include <process/process.h>

// timespec structure for nanosleep
struct timespec {
    int64_t tv_sec;  // seconds
    int64_t tv_nsec; // nanoseconds
};

DECLARE_SYSCALL_HANDLER(nanosleep) {
    /* Linux-style calling convention
     * arg1 = const struct timespec* req (requested time)
     * arg2 = struct timespec* rem (remaining time - for interruption)
     */
    
    const struct timespec* req = reinterpret_cast<const struct timespec*>(arg1);
    struct timespec* rem = reinterpret_cast<struct timespec*>(arg2);
    
    // Validate the request pointer
    if (!req) {
        SYSCALL_TRACE("nanosleep(NULL, 0x%llx) = -EFAULT\n", reinterpret_cast<uint64_t>(rem));
        return -EFAULT;
    }
    
    // Validate the requested time values
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000) {
        SYSCALL_TRACE("nanosleep({tv_sec=%lli, tv_nsec=%lli}, 0x%llx) = -EINVAL\n", 
                      req->tv_sec, req->tv_nsec, reinterpret_cast<uint64_t>(rem));
        return -EINVAL;
    }
    
    // Convert to total nanoseconds
    uint64_t total_ns = req->tv_sec * 1000000000ULL + req->tv_nsec;
    
    // If sleep time is 0, just return success
    if (total_ns == 0) {
        SYSCALL_TRACE("nanosleep({tv_sec=%lli, tv_nsec=%lli}, 0x%llx) = 0\n", 
                      req->tv_sec, req->tv_nsec, reinterpret_cast<uint64_t>(rem));
        return 0;
    }
    
    // For very small sleep times (< 1ms), use the existing nanosleep function
    if (total_ns < 1000000) {
        nanosleep(static_cast<uint32_t>(total_ns));
    }
    // For microsecond range (< 1s), use usleep
    else if (total_ns < 1000000000) {
        usleep(static_cast<uint32_t>(total_ns / 1000));
    }
    // For millisecond range or higher, use msleep for efficiency
    else if (total_ns < 60000000000ULL) { // Less than 60 seconds
        msleep(static_cast<uint32_t>(total_ns / 1000000));
    }
    // For very long sleeps, use sleep in a loop
    else {
        uint32_t seconds = static_cast<uint32_t>(req->tv_sec);
        uint32_t remaining_ns = static_cast<uint32_t>(req->tv_nsec);
        
        // Sleep in chunks of seconds
        sleep(seconds);
        
        // Handle remaining nanoseconds
        if (remaining_ns > 0) {
            if (remaining_ns < 1000000) {
                nanosleep(remaining_ns);
            } else if (remaining_ns < 1000000000) {
                usleep(remaining_ns / 1000);
            } else {
                msleep(remaining_ns / 1000000);
            }
        }
    }
    
    // Clear remaining time if provided (we don't support interruption yet)
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    
    SYSCALL_TRACE("nanosleep({tv_sec=%lli, tv_nsec=%lli}, 0x%llx) = 0\n", 
                  req->tv_sec, req->tv_nsec, reinterpret_cast<uint64_t>(rem));
    
    return 0;
}
