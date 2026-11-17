#include "monitor.hpp"

#include "jobs.hpp"

#include <stlx/proc.h>

bool run_monitor(const monitor_request& request, int& exit_status, failure& failed) {
    int wait_status = 0;
    std::string error;
    if (!run_attached(serial_monitor_words(request), wait_status, error)) {
        failed = { failure_kind::port, error };
        return false;
    }

    exit_status = STLX_WIFEXITED(wait_status) ? STLX_WEXITSTATUS(wait_status) : 1;

    return true;
}
