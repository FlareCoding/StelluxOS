#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <time.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        printf("date: clock_gettime failed\n");
        return 1;
    }

    struct tm* t = gmtime(&ts.tv_sec);
    if (!t) {
        printf("date: gmtime failed\n");
        return 1;
    }

    char buf[64];
    strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S UTC %Y", t);
    printf("%s\n", buf);
    return 0;
}
