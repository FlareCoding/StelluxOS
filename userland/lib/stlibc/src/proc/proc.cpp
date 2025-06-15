#include <stlibc/proc/proc.h>
#include <stlibc/system/syscall.h>

pid_t proc_create(const char* path, uint64_t flags) {
    return static_cast<pid_t>(syscall(SYS_PROC_CREATE, reinterpret_cast<uint64_t>(path), flags, 0, 0, 0));
}

int proc_wait(pid_t pid, int* exit_code) {
    long result = syscall(SYS_PROC_WAIT, pid, reinterpret_cast<uint64_t>(exit_code), 0, 0, 0);
    return static_cast<int>(result);
}
