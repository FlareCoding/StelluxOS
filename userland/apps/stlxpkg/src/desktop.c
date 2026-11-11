#define _POSIX_C_SOURCE 200809L

#include "desktop.h"

#include <stlxconf/conf.h>

#include <signal.h>
#include <stdio.h>
#include <string.h>

#define DM_PID_PATH "/tmp/stlxdm.pid"

bool desktop_reads(const char* path) {
    return strncmp(path, STLXCONF_DROP_IN_DIR "/", strlen(STLXCONF_DROP_IN_DIR "/")) == 0;
}

void desktop_reload(void) {
    FILE* file = fopen(DM_PID_PATH, "r");
    if (!file) {
        return;
    }

    int pid = 0;
    int fields = fscanf(file, "%d", &pid);
    fclose(file);
    if (fields == 1 && pid > 1) {
        kill(pid, SIGHUP);
    }
}
