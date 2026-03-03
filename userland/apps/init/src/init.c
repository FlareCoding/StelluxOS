#include <stlx/proc.h>
#include <stdio.h>
#include <errno.h>

int main(void) {
    int handle = proc_create("/initrd/bin/hello", NULL);
    printf("init: proc_create returned %d (errno=%d)\r\n", handle, errno);
    return 0;
}
