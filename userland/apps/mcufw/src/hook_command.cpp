#include "commands.hpp"

#include "cancel.hpp"
#include "hooks.hpp"
#include "jobs.hpp"

int run_hook_command(const arguments& parsed, reporter& report) {
    hook_command command;
    std::string error;
    if (!parse_hook(parsed.operands, command, error)) {
        return usage_error(error);
    }

    watch_for_cancel();
    bool run_otherwise = false;
    if (!run_hook(command, run_otherwise, error)) {
        report.failed({ failure_kind::recipe, error });
        return 1;
    }

    if (!run_otherwise) {
        return 0;
    }

    int exit_status = 0;
    if (!run_attached(command.otherwise, exit_status, error)) {
        report.failed({ failure_kind::recipe, error });
        return 1;
    }

    std::string problem = exit_problem(exit_status);
    if (!problem.empty()) {
        report.failed({ failure_kind::recipe, command.otherwise[0] + " " + problem });
        return 1;
    }

    return 0;
}
