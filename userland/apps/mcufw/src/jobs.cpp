#include "jobs.hpp"

#include "cancel.hpp"
#include "files.hpp"

#include <poll.h>
#include <signal.h>
#include <stlx/proc.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

static sigset_t cancel_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGPIPE);
    return signals;
}

// poll's result, or -1 with errno EINTR at once when `until_cancel` is set and a cancel arrived
static int wait_for(std::vector<pollfd>& fds, bool until_cancel) {
    sigset_t blocked = cancel_signals();
    sigset_t previous;

    // Blocked from the cancel check until ppoll unblocks them, so a cancel between the two still ends the wait
    sigprocmask(SIG_BLOCK, &blocked, &previous);

    int ready = -1;
    errno = EINTR;
    if (!until_cancel || !cancel_requested()) {
        ready = ppoll(fds.data(), fds.size(), nullptr, &previous);
    }

    int saved = errno;
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    errno = saved;

    return ready;
}

static void wait_for_exit(int handle, int& exit_status) {
    while (proc_wait(handle, &exit_status) != 0 && errno == EINTR) {
    }
}

// Creates the child for `words` without starting it, so its handles can be set first
static bool create_child(const std::vector<std::string>& words, int& handle, std::string& error) {
    if (words.empty()) {
        error = "a command names no program";
        return false;
    }

    std::string program = find_program(words[0]);
    if (program.empty()) {
        bool has_slash = words[0].find('/') != std::string::npos;
        error = has_slash ? words[0] + " is not a program" : "no program named " + words[0] + " is on PATH";
        return false;
    }

    std::vector<const char*> arguments;
    for (size_t i = 1; i < words.size(); i++) {
        arguments.push_back(words[i].c_str());
    }

    arguments.push_back(nullptr);
    handle = proc_create(program.c_str(), arguments.data());
    if (handle < 0) {
        error = "cannot start " + program + ": " + strerror(errno);
        return false;
    }

    return true;
}

bool run_attached(const std::vector<std::string>& words, int& exit_status, std::string& error) {
    int handle = -1;
    if (!create_child(words, handle, error)) {
        return false;
    }

    process_info info = {};
    if (proc_info(handle, &info) != 0 || proc_start(handle) != 0) {
        error = "cannot start " + words[0] + ": " + strerror(errno);
        close(handle);
        return false;
    }

    std::vector<pollfd> exited = { { handle, POLLIN, 0 } };
    bool terminated = false;
    while (wait_for(exited, !terminated) <= 0) {
        if (errno != EINTR) {
            error = std::string("cannot wait for ") + words[0] + ": " + strerror(errno);
            proc_kill(handle);
            wait_for_exit(handle, exit_status);
            return false;
        }

        if (cancel_requested() && !terminated) {
            kill(info.pid, SIGTERM);
            terminated = true;
        }
    }

    wait_for_exit(handle, exit_status);

    return true;
}

std::string exit_problem(int exit_status) {
    if (STLX_WIFSIGNALED(exit_status)) {
        return "was terminated by signal " + std::to_string(STLX_WTERMSIG(exit_status));
    }

    int status = STLX_WEXITSTATUS(exit_status);

    return status == 0 ? std::string() : "exited with status " + std::to_string(status);
}
