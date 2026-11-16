#include "arguments.hpp"
#include "commands.hpp"
#include "layout.hpp"
#include "report.hpp"

#include <cstdio>

constexpr int USAGE_STATUS = 2;
constexpr const char* USAGE = "usage: mcufw boards [<word>] [--events]\n";

int main(int argc, char** argv) {
    arguments parsed;
    std::string error;
    if (!parse_arguments(argc, argv, parsed, error)) {
        fprintf(stderr, "mcufw: %s\n%s", error.c_str(), USAGE);
        return USAGE_STATUS;
    }

    reporter report(parsed.events, stdout, stderr);
    switch (parsed.command) {
    case command_name::boards:
        return run_boards(parsed, system_layout(), report);
    }
}
