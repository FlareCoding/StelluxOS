#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char* argv[]) {
    int count = 1;
    if (argc > 1) {
        count = atoi(argv[1]);
        if (count <= 0) count = 1;
    }

    int sleep_ms = 0;
    if (argc > 2) {
        sleep_ms = atoi(argv[2]);
        if (sleep_ms < 0) sleep_ms = 0;
    }

    struct timespec ts = {
        .tv_sec  = sleep_ms / 1000,
        .tv_nsec = (sleep_ms % 1000) * 1000000L
    };

    setvbuf(stdout, NULL, _IONBF, 0);

    for (int i = 0; i < count; i++) {
        if (sleep_ms > 0) {
            nanosleep(&ts, NULL);
        }
        printf("hello\n");
    }

    return count;
}
