#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>

// Common signal names accepted after a dash, with or without a SIG prefix
static const struct {
    const char* name;
    int         sig;
} signal_names[] = {
    { "HUP",  SIGHUP  }, { "INT",  SIGINT  }, { "QUIT", SIGQUIT },
    { "KILL", SIGKILL }, { "TERM", SIGTERM },
    { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 },
};

static int parse_signal(const char* spec) {
    if (spec[0] >= '0' && spec[0] <= '9') {
        char* end = NULL;
        long sig = strtol(spec, &end, 10);
        if (*end != '\0' || sig < 0 || sig > 64) {
            return -1;
        }

        return (int)sig;
    }

    if (strncmp(spec, "SIG", 3) == 0) {
        spec += 3;
    }
    for (size_t i = 0; i < sizeof(signal_names) / sizeof(signal_names[0]); i++) {
        if (strcmp(spec, signal_names[i].name) == 0) {
            return signal_names[i].sig;
        }
    }

    return -1;
}

int main(int argc, char* argv[]) {
    int sig = SIGTERM;
    int argi = 1;

    if (argi < argc && argv[argi][0] == '-' && strcmp(argv[argi], "--") != 0) {
        sig = parse_signal(argv[argi] + 1);
        if (sig < 0) {
            fprintf(stderr, "kill: invalid signal: %s\n", argv[argi]);
            return 1;
        }

        argi++;
    }

    // A -- terminator lets negative process-group pids follow
    if (argi < argc && strcmp(argv[argi], "--") == 0) {
        argi++;
    }

    if (argi >= argc) {
        fprintf(stderr, "usage: kill [-SIGNAL] [--] pid...\n");
        return 1;
    }

    int status = 0;
    for (; argi < argc; argi++) {
        char* end = NULL;
        long pid = strtol(argv[argi], &end, 10);
        if (argv[argi][0] == '\0' || *end != '\0' || pid == 0) {
            fprintf(stderr, "kill: invalid pid: %s\n", argv[argi]);
            status = 1;
            continue;
        }

        if (kill((pid_t)pid, sig) != 0) {
            fprintf(stderr, "kill: (%ld): %s\n", pid, strerror(errno));
            status = 1;
        }
    }

    return status;
}
