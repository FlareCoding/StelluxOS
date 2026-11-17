/* arguments, run on the build machine: each command's operands and options, both forms of an option's value, `hook`
 * taking every word after it, and the command lines that fail as usage errors */

#include "arguments.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;
constexpr const char* JOBS_MISTAKE = "--jobs takes how many compiles run at once, 1 to 85, or 0 for one per CPU";

/* A command line the parser refuses, with the reason it gives */
struct usage_error {
    std::vector<const char*> words;
    const char*              error;
};

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static bool parse(std::vector<const char*> words, arguments& parsed, std::string& error) {
    words.insert(words.begin(), "mcufw");
    std::vector<char*> argv;
    for (const char* word : words) {
        argv.push_back(const_cast<char*>(word));
    }

    return parse_arguments(static_cast<int>(argv.size()), argv.data(), parsed, error);
}

static void check_boards() {
    arguments parsed;
    std::string error;
    check(parse({ "boards" }, parsed, error) && parsed.command == command_name::boards, "boards parses alone");
    check(parsed.operands.empty() && !parsed.events, "boards alone has no word and no events");

    check(parse({ "boards", "--events", "esp32da" }, parsed, error), "boards parses with a word and --events");
    check(parsed.operands == std::vector<std::string>({ "esp32da" }) && parsed.events,
          "the word and --events are kept");

    check(parse({ "install", "esp32:esp32", "esp32:esp32:esp32da" }, parsed, error) &&
              parsed.command == command_name::install && parsed.operands.size() == 2,
          "install takes several targets");
    check(parse({ "remove", "esp32:esp32" }, parsed, error) && parsed.command == command_name::remove,
          "remove parses with a target");
}

static void check_hook() {
    arguments parsed;
    std::string error;
    std::vector<std::string> words = { "copy-first", "a", "b", "--else", "x", "--events", "-DX" };
    check(parse({ "hook", "copy-first", "a", "b", "--else", "x", "--events", "-DX" }, parsed, error) &&
              parsed.command == command_name::hook && parsed.operands == words && !parsed.events,
          "hook takes every word after it as an operand");
}

static void check_build() {
    arguments parsed;
    std::string error;
    check(parse({ "build" }, parsed, error) && parsed.command == command_name::build && parsed.operands.empty() &&
              parsed.jobs == 0 && parsed.warnings == "default" && !parsed.verbose && !parsed.events,
          "a bare build takes the defaults");

    bool parsed_all = parse({ "build", "Lamp", "--board", "esp32da", "--jobs", "4", "--warnings", "all", "--verbose",
                              "--events" },
                            parsed, error);
    check(parsed_all && parsed.operands == std::vector<std::string>{ "Lamp" } && parsed.board == "esp32da" &&
              parsed.jobs == 4 && parsed.warnings == "all" && parsed.verbose && parsed.events,
          "every build option is read");

    check(parse({ "build", "--board=esp32da:FlashSize=8M", "--jobs=0", "--warnings=more" }, parsed, error) &&
              parsed.board == "esp32da:FlashSize=8M" && parsed.jobs == 0 && parsed.warnings == "more",
          "an option's value may follow an equals sign");

    check(parse({ "build", "--jobs", "85" }, parsed, error) && parsed.jobs == 85,
          "85 compiles at once, the most the kernel's poll lets a build wait on");
}

static void check_clean() {
    arguments parsed;
    std::string error;
    check(parse({ "clean" }, parsed, error) && parsed.command == command_name::clean && parsed.operands.empty(),
          "a bare clean takes no sketch");
    check(parse({ "clean", "/sketches/Gone/Gone.ino" }, parsed, error) &&
              parsed.operands == std::vector<std::string>{ "/sketches/Gone/Gone.ino" },
          "clean takes a sketch that may be gone");
}

static void check_usage_errors() {
    const usage_error usage_errors[] = {
        { {}, "no command given" },
        { { "flash" }, "unknown command flash" },
        { { "boards", "--json" }, "boards takes no option --json" },
        { { "boards", "esp32da", "esp32s3" }, "boards takes 1 word at most" },
        { { "install" }, "install needs a board, a core or a library, such as FastLED" },
        { { "remove", "--force" }, "remove takes no option --force" },
        { { "remove" }, "remove needs a core or a library, such as FastLED" },
        { { "build", "a", "b" }, "build takes 1 sketch at most" },
        { { "build", "--port", "x" }, "build takes no option --port" },
        { { "build", "--board" }, "--board needs a value" },
        { { "build", "--jobs", "x" }, JOBS_MISTAKE },
        { { "build", "--jobs", "86" }, JOBS_MISTAKE },
        { { "build", "--jobs", "-1" }, JOBS_MISTAKE },
        { { "build", "--warnings", "loud" }, "--warnings takes none, default, more or all" },
        { { "clean", "a", "b" }, "clean takes 1 sketch at most" },
        { { "clean", "--jobs", "4" }, "clean takes no option --jobs" },
    };

    for (const usage_error& expected : usage_errors) {
        arguments parsed;
        std::string error;
        check(!parse(expected.words, parsed, error) && error == expected.error, expected.error);
    }
}

int main() {
    check_boards();
    check_build();
    check_hook();
    check_clean();
    check_usage_errors();

    if (g_failures != 0) {
        printf("arguments: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("arguments: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
