#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        setvbuf(stdout, NULL, _IONBF, 0);
        printf("sleep: missing operand\n");
        return 1;
    }

    int seconds = atoi(argv[1]);
    if (seconds <= 0) return 0;

    struct timespec ts = { .tv_sec = seconds, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
    return 0;
}
