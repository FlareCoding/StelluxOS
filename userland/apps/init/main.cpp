// #include <sched/sched.h>
// #include <time/time.h>
// #include <dynpriv/dynpriv.h>
// #include <ipc/shm.h>
// #include <core/klog.h>

#include <stlibc.h>

int main() {
    // const auto proc_flags =
    //     process_creation_flags::IS_USERLAND     |
    //     process_creation_flags::SCHEDULE_NOW    |
    //     process_creation_flags::ALLOW_ELEVATE;

    // if (!create_process("/initrd/bin/shell", proc_flags)) {
    //     return -1;
    // }

    sys_write("sys_write executed\n");

    return 0;
}
