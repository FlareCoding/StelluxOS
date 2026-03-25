#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int n = 100000;
    if (argc > 1) {
        n = atoi(argv[1]);
        if (n <= 0) n = 100000;
    }

    struct timespec start, end, ts;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        printf("clockbench: clock_gettime failed\n");
        return 1;
    }

    for (int i = 0; i < n; i++) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        printf("clockbench: clock_gettime failed\n");
        return 1;
    }

    long long elapsed_ns = (long long)(end.tv_sec - start.tv_sec) * 1000000000LL
                         + (end.tv_nsec - start.tv_nsec);
    long long avg_ns = elapsed_ns / n;
    long long elapsed_us = elapsed_ns / 1000;

    printf("%d calls in %lld.%03lld ms (avg %lld ns/call)\n",
           n, elapsed_us / 1000, elapsed_us % 1000, avg_ns);
    return 0;
}
