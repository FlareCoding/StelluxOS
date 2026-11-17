#ifndef MCUFW_JOBS_HPP
#define MCUFW_JOBS_HPP

// Runs commands as child processes through Stellux's native process calls, finding a program without a slash on PATH.
// A child takes mcufw's working directory when it is created.

#include <cstdint>
#include <string>
#include <vector>

// The most children run at once, since the kernel polls at most 256 descriptors in one call and each child takes three:
// its handle, its stdout and its stderr
constexpr uint32_t JOBS_MAX = 85;

struct job {
    uint64_t                 id;                 // the caller's number for it, given back in its `finished_job`
    std::vector<std::string> words;              // the program first, then its arguments
    std::string              working_directory;  // empty for mcufw's own
};

struct finished_job {
    uint64_t    id;
    int         exit_status;   // as proc_wait gives it
    std::string output;        // stdout
    std::string error_output;  // stderr
};

// Runs up to a set number of children at once, each with stdin on /dev/null, and hands over each one's output whole
// once it exits, so children running side by side never interleave theirs
class job_runner {
public:
    explicit job_runner(uint32_t max_running);

    // Kills every child still running and waits for it
    ~job_runner();

    job_runner(const job_runner&) = delete;
    job_runner& operator=(const job_runner&) = delete;

    uint32_t free_slots() const;
    bool     idle() const;

    // Fails when no slot is free or the program cannot start, `E2BIG` among the reasons for a command too long
    bool start(const job& job, std::string& error);

    // Waits for the next child to exit, so a caller must have one running: with none it waits for a cancel. Fails when
    // poll fails, or once a cancel signal arrived, after killing every child and waiting for it.
    bool wait_for_next(finished_job& finished, std::string& error);

    // Kills every running child and waits for it
    void stop_all();

private:
    struct running_job {
        uint64_t    id;
        int         handle;        // the process handle, which reads as ready once the child exits
        int         output_pipe;   // the read end of the child's stdout
        int         error_pipe;    // the read end of its stderr
        std::string output;
        std::string error_output;
    };

    uint32_t                 m_max_running;
    std::vector<running_job> m_running;
};

// Runs `words` with mcufw's stdin, stdout and stderr and waits for it. A cancel sends the child SIGTERM and keeps
// waiting, so a child that owns the terminal can restore it.
bool run_attached(const std::vector<std::string>& words, int& exit_status, std::string& error);

// How a child ended, from `exit_status` as proc_wait gives it: empty for status 0, else what went wrong
std::string exit_problem(int exit_status);

// How many CPUs this process may run on, from sched_getaffinity, or 1 when that fails, at most `JOBS_MAX`
uint32_t cpu_count();

#endif // MCUFW_JOBS_HPP
