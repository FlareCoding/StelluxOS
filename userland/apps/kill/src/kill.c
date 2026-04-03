#include <stdio.h>
#include <stdlib.h>
#include <stlx/proc.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: kill <tid>\n");
        return 1;
    }

    int tid = atoi(argv[1]);
    if (tid <= 0) {
        fprintf(stderr, "kill: invalid tid: %s\n", argv[1]);
        return 1;
    }

    int rc = proc_kill_tid(tid);
    if (rc < 0) {
        fprintf(stderr, "kill: failed to kill tid %d (error %d)\n", tid, rc);
        return 1;
    }

    return 0;
}
