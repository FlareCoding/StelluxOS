#include <stlx/proc.h>
#include <stdio.h>
#include <errno.h>

int main(void) {
    const char* argv[] = { "4", "1000", NULL };
    int handle = proc_create("/initrd/bin/hello", argv);
    if (handle < 0) {
        printf("init: proc_create failed (errno=%d)\r\n", errno);
        return 1;
    }
    printf("init: proc_create returned handle %d\r\n", handle);

    int err = proc_start(handle);
    if (err < 0) {
        printf("init: proc_start failed (errno=%d)\r\n", errno);
        return 2;
    }
    printf("init: proc_start ok\r\n");

    int exit_code = -1;
    err = proc_wait(handle, &exit_code);
    if (err < 0) {
        printf("init: proc_wait failed (errno=%d)\r\n", errno);
        return 3;
    }
    printf("init: child exited with code %d\r\n", exit_code);
    return 0;
}
