#include <stdio.h>
#include <sys/utsname.h>

int main(void) {
    struct utsname buf;
    if (uname(&buf) != 0) {
        fprintf(stderr, "hostname: uname syscall failed\n");
        return 1;
    }
    puts(buf.nodename);
    return 0;
}
