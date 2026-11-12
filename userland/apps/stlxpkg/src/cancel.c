#define _POSIX_C_SOURCE 200809L

#include "cancel.h"
#include "report.h"

#include <signal.h>
#include <stddef.h>

typedef struct {
    int         number;
    const char* name;
} cancel_signal;

static const cancel_signal CANCEL_SIGNALS[] = {
    { SIGTERM, "SIGTERM" },
    { SIGINT, "SIGINT" },
    { SIGPIPE, "SIGPIPE" },
};

static volatile sig_atomic_t g_signal;

static void on_signal(int number) {
    if (g_signal == 0) {
        g_signal = number;
    }
}

/* Without SA_RESTART, a signal also ends a wait for the network at once */
void cancel_watch(void) {
    struct sigaction action = { .sa_handler = on_signal };
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(CANCEL_SIGNALS) / sizeof(CANCEL_SIGNALS[0]); i++) {
        sigaction(CANCEL_SIGNALS[i].number, &action, NULL);
    }
}

int cancel_check(void) {
    for (size_t i = 0; i < sizeof(CANCEL_SIGNALS) / sizeof(CANCEL_SIGNALS[0]); i++) {
        if (CANCEL_SIGNALS[i].number == g_signal) {
            return fail(FAILURE_CANCELLED, "stopped by %s", CANCEL_SIGNALS[i].name);
        }
    }

    return 0;
}
