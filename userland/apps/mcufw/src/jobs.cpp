#include "jobs.hpp"

#include "cancel.hpp"
#include "files.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stlx/proc.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>

constexpr size_t READ_CHUNK = 4096;
constexpr uint64_t NANOSECONDS_PER_SECOND = 1'000'000'000;

static sigset_t cancel_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGPIPE);
    return signals;
}

// poll's result, 0 once `timeout` passed when one is given, or -1 with errno EINTR at once when `until_cancel` is
// set and a cancel arrived
static int wait_for(std::vector<pollfd>& fds, bool until_cancel, const timespec* timeout = nullptr) {
    sigset_t blocked = cancel_signals();
    sigset_t previous;

    // Blocked from the cancel check until ppoll unblocks them, so a cancel between the two still ends the wait
    sigprocmask(SIG_BLOCK, &blocked, &previous);

    int ready = -1;
    errno = EINTR;
    if (!until_cancel || !cancel_requested()) {
        ready = ppoll(fds.data(), fds.size(), timeout, &previous);
    }

    int saved = errno;
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    errno = saved;

    return ready;
}

static void close_if_open(int& fd) {
    if (fd >= 0) {
        close(fd);
    }

    fd = -1;
}

static void wait_for_exit(int handle, int& exit_status) {
    while (proc_wait(handle, &exit_status) != 0 && errno == EINTR) {
    }
}

// Creates the child for `words` without starting it, so its handles can be set first, changing into
// `working_directory` around `proc_create`
static bool create_child(const std::vector<std::string>& words, const std::string& working_directory, int& handle,
                         std::string& error) {
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

    char own_directory[PATH_MAX];
    if (!working_directory.empty()) {
        if (!getcwd(own_directory, sizeof(own_directory)) || chdir(working_directory.c_str()) != 0) {
            error = "cannot run " + program + " in " + working_directory + ": " + strerror(errno);
            return false;
        }
    }

    handle = proc_create(program.c_str(), arguments.data());
    int create_error = errno;

    if (!working_directory.empty() && chdir(own_directory) != 0) {
        error = std::string("cannot return to ") + own_directory + ": " + strerror(errno);
        if (handle >= 0) {
            close(handle);
        }

        return false;
    }

    if (handle < 0) {
        error = "cannot start " + program + ": " + strerror(create_error);
        return false;
    }

    return true;
}

// Reads what `fd` holds without waiting, so a grandchild still holding the pipe cannot stall a finished job
static void drain(int fd, std::string& text) {
    char buffer[READ_CHUNK];
    while (fd >= 0) {
        pollfd readable = { fd, POLLIN, 0 };
        if (poll(&readable, 1, 0) <= 0 || !(readable.revents & (POLLIN | POLLHUP))) {
            return;
        }

        ssize_t length = read(fd, buffer, sizeof(buffer));
        if (length < 0 && errno == EINTR) {
            continue;
        }

        if (length <= 0) {
            return;
        }

        text.append(buffer, static_cast<size_t>(length));
    }
}

job_runner::job_runner(uint32_t max_running)
    : m_max_running(max_running == 0 ? 1 : max_running) {}

job_runner::~job_runner() {
    stop_all();
}

uint32_t job_runner::free_slots() const {
    return m_max_running - static_cast<uint32_t>(m_running.size());
}

bool job_runner::idle() const {
    return m_running.empty();
}

bool job_runner::start(const job& job, std::string& error) {
    if (free_slots() == 0) {
        error = "every job slot is taken";
        return false;
    }

    int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int output[2] = { -1, -1 };
    int errors[2] = { -1, -1 };
    bool piped = input >= 0 && pipe2(output, O_CLOEXEC) == 0 && pipe2(errors, O_CLOEXEC) == 0;
    if (!piped) {
        error = std::string("cannot make a child's pipes: ") + strerror(errno);
    }

    int handle = -1;
    bool started = piped && create_child(job.words, job.working_directory, handle, error);
    if (started) {
        started = proc_set_handle(handle, STDIN_FILENO, input) == 0 &&
                  proc_set_handle(handle, STDOUT_FILENO, output[1]) == 0 &&
                  proc_set_handle(handle, STDERR_FILENO, errors[1]) == 0 && proc_start(handle) == 0;
        if (!started) {
            error = "cannot start " + job.words[0] + ": " + strerror(errno);
            close(handle);
        }
    }

    close_if_open(input);
    close_if_open(output[1]);
    close_if_open(errors[1]);
    if (!started) {
        close_if_open(output[0]);
        close_if_open(errors[0]);
        return false;
    }

    m_running.push_back({ job.id, handle, output[0], errors[0], {}, {} });
    return true;
}

bool job_runner::wait_for_next(finished_job& finished, std::string& error) {
    std::vector<pollfd> fds;
    while (true) {
        fds.clear();
        for (const running_job& running : m_running) {
            fds.push_back({ running.handle, POLLIN, 0 });
            fds.push_back({ running.output_pipe, POLLIN, 0 });
            fds.push_back({ running.error_pipe, POLLIN, 0 });
        }

        if (wait_for(fds, true) < 0) {
            int wait_error = errno;
            if (wait_error == EINTR && !cancel_requested()) {
                continue;
            }

            stop_all();
            if (wait_error == EINTR) {
                error = "the run was cancelled";
            } else {
                error = "cannot wait for children: " + std::string(strerror(wait_error));
            }

            return false;
        }

        char buffer[READ_CHUNK];
        for (size_t i = 0; i < m_running.size(); i++) {
            running_job& running = m_running[i];
            int* pipes[] = { &running.output_pipe, &running.error_pipe };
            std::string* texts[] = { &running.output, &running.error_output };
            for (size_t which = 0; which < 2; which++) {
                if (*pipes[which] < 0 || !(fds[i * 3 + 1 + which].revents & (POLLIN | POLLHUP | POLLERR))) {
                    continue;
                }

                ssize_t length = read(*pipes[which], buffer, sizeof(buffer));
                if (length > 0) {
                    texts[which]->append(buffer, static_cast<size_t>(length));
                } else if (length == 0 || errno != EINTR) {
                    close_if_open(*pipes[which]);
                }
            }
        }

        for (size_t i = 0; i < m_running.size(); i++) {
            if (!(fds[i * 3].revents & POLLIN)) {
                continue;
            }

            running_job& running = m_running[i];
            drain(running.output_pipe, running.output);
            drain(running.error_pipe, running.error_output);
            close_if_open(running.output_pipe);
            close_if_open(running.error_pipe);

            finished = { running.id, 0, std::move(running.output), std::move(running.error_output) };
            wait_for_exit(running.handle, finished.exit_status);
            m_running.erase(m_running.begin() + static_cast<ptrdiff_t>(i));
            return true;
        }
    }
}

void job_runner::stop_all() {
    for (running_job& running : m_running) {
        proc_kill(running.handle);
    }

    for (running_job& running : m_running) {
        int exit_status = 0;
        wait_for_exit(running.handle, exit_status);
        close_if_open(running.output_pipe);
        close_if_open(running.error_pipe);
    }

    m_running.clear();
}

bool run_attached(const std::vector<std::string>& words, int& exit_status, std::string& error) {
    int handle = -1;
    if (!create_child(words, "", handle, error)) {
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

bool run_streamed(const std::vector<std::string>& words, bool stderr_to_output, uint64_t quiet_ns,
                  const std::function<void(std::string_view bytes)>& on_output, int& exit_status, std::string& error) {
    int handle = -1;
    if (!create_child(words, "", handle, error)) {
        return false;
    }

    int output[2];
    if (pipe2(output, O_CLOEXEC) != 0) {
        error = "cannot make a pipe for " + words[0] + ": " + strerror(errno);
        close(handle);
        return false;
    }

    process_info info = {};
    bool started = proc_set_handle(handle, STDOUT_FILENO, output[1]) == 0 &&
                   (!stderr_to_output || proc_set_handle(handle, STDERR_FILENO, output[1]) == 0) &&
                   proc_info(handle, &info) == 0 && proc_start(handle) == 0;
    int start_error = errno;
    close(output[1]);
    if (!started) {
        error = "cannot start " + words[0] + ": " + strerror(start_error);
        close(output[0]);
        close(handle);
        return false;
    }

    // The child's exit ends the reading, so a grandchild that keeps the pipe open cannot hold the run
    std::vector<pollfd> watched = { { output[0], POLLIN, 0 }, { handle, POLLIN, 0 } };
    timespec quiet = { static_cast<time_t>(quiet_ns / NANOSECONDS_PER_SECOND),
                       static_cast<long>(quiet_ns % NANOSECONDS_PER_SECOND) };
    bool is_silent = true;

    bool terminated = false;
    bool is_waited = true;
    char buffer[READ_CHUNK];
    while (true) {
        int ready = wait_for(watched, !terminated, quiet_ns > 0 && !is_silent ? &quiet : nullptr);
        if (ready == 0) {
            on_output({});
            is_silent = true;
            continue;
        }

        if (ready < 0) {
            if (errno != EINTR) {
                error = std::string("cannot wait for ") + words[0] + ": " + strerror(errno);
                proc_kill(handle);
                is_waited = false;
                break;
            }

            if (cancel_requested() && !terminated) {
                kill(info.pid, SIGTERM);
                terminated = true;
            }

            continue;
        }

        if (watched[1].revents & POLLIN) {
            std::string rest;
            drain(output[0], rest);
            if (!rest.empty()) {
                on_output(rest);
            }

            break;
        }

        ssize_t length = read(output[0], buffer, sizeof(buffer));
        if (length > 0) {
            on_output(std::string_view(buffer, static_cast<size_t>(length)));
            is_silent = false;
        } else if (length == 0 || errno != EINTR) {
            watched[0].fd = -1;
        }
    }

    close(output[0]);
    wait_for_exit(handle, exit_status);
    return is_waited;
}

std::string exit_problem(int exit_status) {
    if (STLX_WIFSIGNALED(exit_status)) {
        return "was terminated by signal " + std::to_string(STLX_WTERMSIG(exit_status));
    }

    int status = STLX_WEXITSTATUS(exit_status);

    return status == 0 ? std::string() : "exited with status " + std::to_string(status);
}

uint32_t cpu_count() {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    if (sched_getaffinity(0, sizeof(cpus), &cpus) != 0) {
        return 1;
    }

    int count = CPU_COUNT(&cpus);

    return count > 0 ? std::min(static_cast<uint32_t>(count), JOBS_MAX) : 1;
}
