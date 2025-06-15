#include <stlibc/proc/pid.h>
#include <stlibc/system/syscall.h>

pid_t getpid() {
    return static_cast<pid_t>(syscall(SYS_GETPID, 0, 0, 0, 0, 0));
}
