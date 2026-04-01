#include <stdio.h>
#include <sys/utsname.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    struct utsname buf;
    if (uname(&buf) != 0) {
        fprintf(stderr, "hostname: uname syscall failed\n");
        return 1;
    }
    puts(buf.nodename);
    return 0;
}
