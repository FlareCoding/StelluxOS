/* arguments, run on the build machine: each command with its words and `--events`, and the command lines that fail as
 * usage errors */

#include "arguments.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

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

static void check_usage_errors() {
    const usage_error usage_errors[] = {
        { {}, "no command given" },
        { { "flash" }, "unknown command flash" },
        { { "boards", "--json" }, "boards takes no option --json" },
        { { "boards", "esp32da", "esp32s3" }, "boards takes 1 word at most" },
        { { "install" }, "install needs a board, a core or a library, such as FastLED" },
        { { "remove", "--force" }, "remove takes no option --force" },
        { { "remove" }, "remove needs a core or a library, such as FastLED" },
    };

    for (const usage_error& expected : usage_errors) {
        arguments parsed;
        std::string error;
        check(!parse(expected.words, parsed, error) && error == expected.error, expected.error);
    }
}

int main() {
    check_boards();
    check_usage_errors();

    if (g_failures != 0) {
        printf("arguments: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("arguments: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
