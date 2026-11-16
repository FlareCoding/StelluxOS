#include "cancel.hpp"

#include <csignal>
#include <initializer_list>

static volatile sig_atomic_t g_cancel_requested = 0;

static void request_cancel(int) {
    g_cancel_requested = 1;
}

void watch_for_cancel() {
    struct sigaction action {};
    action.sa_handler = request_cancel;
    sigemptyset(&action.sa_mask);

    for (int signal_number : { SIGINT, SIGTERM, SIGPIPE }) {
        sigaction(signal_number, &action, nullptr);
    }
}

bool cancel_requested() {
    return g_cancel_requested != 0;
}
