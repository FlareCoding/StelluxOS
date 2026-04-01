#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <time.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "uptime: clock_gettime failed\n");
        return 1;
    }

    long total = (long)ts.tv_sec;
    long days    = total / 86400;
    long hours   = (total % 86400) / 3600;
    long minutes = (total % 3600) / 60;
    long seconds = total % 60;

    printf("up ");
    if (days > 0)
        printf("%ld day%s, ", days, days == 1 ? "" : "s");
    if (days > 0 || hours > 0)
        printf("%ld hour%s, ", hours, hours == 1 ? "" : "s");
    if (days > 0 || hours > 0 || minutes > 0)
        printf("%ld minute%s, ", minutes, minutes == 1 ? "" : "s");
    printf("%ld second%s\n", seconds, seconds == 1 ? "" : "s");

    return 0;
}
