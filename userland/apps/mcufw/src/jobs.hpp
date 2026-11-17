#ifndef MCUFW_JOBS_HPP
#define MCUFW_JOBS_HPP

// Runs commands as child processes through Stellux's native process calls, finding a program without a slash on PATH.
// A child takes mcufw's working directory when it is created.

#include <string>
#include <vector>

// Runs `words` with mcufw's stdin, stdout and stderr and waits for it. A cancel sends the child SIGTERM and keeps
// waiting, so a child that owns the terminal can restore it.
bool run_attached(const std::vector<std::string>& words, int& exit_status, std::string& error);

// How a child ended, from `exit_status` as proc_wait gives it: empty for status 0, else what went wrong
std::string exit_problem(int exit_status);

#endif // MCUFW_JOBS_HPP
