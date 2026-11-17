#include "arguments.hpp"

#include "jobs.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>

// A command's name, how many operands it takes and what they name, with the sentence for too few
struct command_rule {
    std::string_view name;
    command_name     command;
    size_t           operands_max;
    std::string_view operand;          // what an operand names, for "takes one <operand> at most"
    std::string_view missing_operand;  // empty when the command needs none
};

constexpr command_rule COMMAND_RULES[] = {
    { "boards", command_name::boards, 1, "word", "" },
    { "install", command_name::install, SIZE_MAX, "", "install needs a board, a core or a library, such as FastLED" },
    { "remove", command_name::remove, SIZE_MAX, "", "remove needs a core or a library, such as FastLED" },
    { "build", command_name::build, 1, "sketch", "" },
    { "clean", command_name::clean, 1, "sketch", "" },
    { "flash", command_name::flash, 1, "sketch", "" },
    { "monitor", command_name::monitor, 1, "sketch", "" },
};

// arduino-cli's warning levels, each a `compiler.warning_flags.<level>` key
constexpr const char* WARNING_LEVELS[] = { "none", "default", "more", "all" };

static const command_rule* find_rule(std::string_view name) {
    for (const command_rule& rule : COMMAND_RULES) {
        if (rule.name == name) {
            return &rule;
        }
    }

    return nullptr;
}

static bool parse_jobs(std::string_view text, uint32_t& jobs) {
    const char* end = text.data() + text.size();
    std::from_chars_result parsed = std::from_chars(text.data(), end, jobs);
    return !text.empty() && parsed.ec == std::errc() && parsed.ptr == end && jobs <= JOBS_MAX;
}

static bool is_warning_level(std::string_view text) {
    for (const char* level : WARNING_LEVELS) {
        if (text == level) {
            return true;
        }
    }

    return false;
}

static bool is_option(std::string_view word, std::string_view name) {
    return word == name || (word.starts_with(name) && word.size() > name.size() && word[name.size()] == '=');
}

// The value of `--name value` or `--name=value`, leaving `index` on the value's word
static bool option_value(int argc, char** argv, int& index, std::string_view name, std::string& value,
                         std::string& error) {
    std::string_view word = argv[index];
    if (word.size() > name.size() && word[name.size()] == '=') {
        value = word.substr(name.size() + 1);
        return true;
    }

    if (index + 1 >= argc) {
        error = std::string(name) + " needs a value";
        return false;
    }

    index++;
    value = argv[index];
    return true;
}

static bool parse_build_option(int argc, char** argv, int& index, arguments& parsed, std::string& error) {
    std::string_view word = argv[index];
    std::string value;
    if (is_option(word, "--board")) {
        return option_value(argc, argv, index, "--board", parsed.board, error);
    }

    if (is_option(word, "--jobs")) {
        if (!option_value(argc, argv, index, "--jobs", value, error)) {
            return false;
        }

        if (!parse_jobs(value, parsed.jobs)) {
            error = "--jobs takes how many compiles run at once, 1 to 85, or 0 for one per CPU";
            return false;
        }

        return true;
    }

    if (!option_value(argc, argv, index, "--warnings", value, error)) {
        return false;
    }

    if (!is_warning_level(value)) {
        error = "--warnings takes none, default, more or all";
        return false;
    }

    parsed.warnings = value;
    return true;
}

// `--port <port>` or `--baud <rate>`, the options of the commands that open a serial port
static bool parse_port_option(int argc, char** argv, int& index, arguments& parsed, std::string& error) {
    if (is_option(argv[index], "--port")) {
        return option_value(argc, argv, index, "--port", parsed.port, error);
    }

    std::string value;
    if (!option_value(argc, argv, index, "--baud", value, error)) {
        return false;
    }

    const char* end = value.data() + value.size();
    std::from_chars_result read = std::from_chars(value.data(), end, parsed.baud);
    if (value.empty() || read.ec != std::errc() || read.ptr != end || parsed.baud == 0) {
        error = "--baud takes a speed in baud from 1 to 4294967295";
        return false;
    }

    return true;
}

bool parse_arguments(int argc, char** argv, arguments& parsed, std::string& error) {
    if (argc < 2) {
        error = "no command given";
        return false;
    }

    parsed = {};
    if (std::string_view(argv[1]) == "hook") {
        parsed.command = command_name::hook;
        parsed.operands.assign(argv + 2, argv + argc);
        return true;
    }

    const command_rule* rule = find_rule(argv[1]);
    if (!rule) {
        error = "unknown command " + std::string(argv[1]);
        return false;
    }

    parsed.command = rule->command;
    bool is_flash = rule->command == command_name::flash;
    bool opens_port = is_flash || rule->command == command_name::monitor;
    for (int index = 2; index < argc; index++) {
        std::string_view word = argv[index];
        bool is_build_option = is_option(word, "--board") || is_option(word, "--jobs") || is_option(word, "--warnings");
        bool is_port_option = is_option(word, "--port") || is_option(word, "--baud");
        if (word == "--events") {
            parsed.events = true;
        } else if (word == "--verbose") {
            parsed.verbose = true;
        } else if ((rule->command == command_name::build || is_flash) && is_build_option) {
            if (!parse_build_option(argc, argv, index, parsed, error)) {
                return false;
            }
        } else if (rule->command == command_name::clean && word == "--cores") {
            parsed.cores = true;
        } else if (opens_port && is_port_option) {
            if (!parse_port_option(argc, argv, index, parsed, error)) {
                return false;
            }
        } else if (is_flash && word == "--monitor") {
            parsed.monitor = true;
        } else if (opens_port && word == "--") {
            parsed.serial_options.assign(argv + index + 1, argv + argc);
            break;
        } else if (word.starts_with("-")) {
            error = std::string(rule->name) + " takes no option " + std::string(word);
            return false;
        } else {
            parsed.operands.emplace_back(word);
        }
    }

    if (parsed.operands.empty() && !rule->missing_operand.empty()) {
        error = rule->missing_operand;
        return false;
    }

    if (parsed.operands.size() > rule->operands_max) {
        error = std::string(rule->name) + " takes " + std::to_string(rule->operands_max) + " " +
                std::string(rule->operand) + " at most";
        return false;
    }

    bool starts_monitor = rule->command == command_name::monitor || parsed.monitor;
    if (starts_monitor && parsed.events) {
        error = "the monitor prints no events, so a program runs serial monitor --events on the port itself";
        return false;
    }

    if (is_flash && !parsed.monitor && !parsed.serial_options.empty()) {
        error = "the words after -- go to serial monitor, which flash starts only with --monitor";
        return false;
    }

    return true;
}
