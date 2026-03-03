#include <stlx/proc.h>
#include <stdio.h>
#include <errno.h>

int main(void) {
    int handle = proc_create("/initrd/bin/hello", NULL);
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

    err = proc_detach(handle);
    if (err < 0) {
        printf("init: proc_detach failed (errno=%d)\r\n", errno);
        return 3;
    }
    printf("init: proc_detach ok, hello is independent\r\n");
    return 0;
}
