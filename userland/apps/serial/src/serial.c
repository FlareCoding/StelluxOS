/* Lists the serial ports, watches them come and go, and connects the terminal to one */
#include "list.h"
#include "monitor.h"
#include "options.h"

#include <stdio.h>

#define PROBLEM_MAX       256
#define USAGE_EXIT_STATUS 2

int main(int argc, char* argv[]) {
    serial_args args;
    char problem[PROBLEM_MAX];
    if (options_parse(argc - 1, argv + 1, &args, problem, sizeof(problem)) != 0) {
        fprintf(stderr, "serial: %s\n\n%s", problem, options_usage());
        return USAGE_EXIT_STATUS;
    }

    if (args.command == COMMAND_MONITOR) {
        return run_monitor(&args);
    }

    return args.command == COMMAND_LIST ? run_list(&args) : run_watch(&args);
}
