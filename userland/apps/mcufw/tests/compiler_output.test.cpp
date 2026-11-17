/* compiler_output, run on the build machine: GCC's messages read as problems, with and without a column, fatal errors
 * as errors, and every other line left out */

#include "compiler_output.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

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

static bool problem_is(const compiler_problem& problem, problem_severity severity, const std::string& file,
                       uint32_t line, uint32_t column, const std::string& message) {
    return problem.severity == severity && problem.file == file && problem.line == line && problem.column == column &&
           problem.message == message;
}

static void check_problems() {
    std::string output = "/sketches/WiFiScan/WiFiScan.ino: In function 'void setup()':\n"
                         "/sketches/WiFiScan/WiFiScan.ino:10:23: error: expected ';' before 'WiFi'\n"
                         "   10 |   Serial.begin(115200)\n"
                         "      |                       ^\n"
                         "In file included from /sketches/Lamp/Lamp.ino:1:\n"
                         "/libraries/Strip/src/Strip.h:3:10: fatal error: Missing.h: No such file or directory\r\n"
                         "/sketches/Lamp/Lamp.ino:5: warning: unused variable 'x' [-Wunused-variable]\n"
                         "/sketches/Lamp/Lamp.ino:7:1: note: in expansion of macro 'GLOW'\n"
                         "collect2: error: ld returned 1 exit status\n"
                         "/sketches/Lamp/Lamp.ino:9:2: error: a message: with error: inside it";
    std::vector<compiler_problem> problems = find_compiler_problems(output);
    check(problems.size() == 5, "five lines name problems");
    if (problems.size() != 5) {
        return;
    }

    check(problem_is(problems[0], problem_severity::error, "/sketches/WiFiScan/WiFiScan.ino", 10, 23,
                     "expected ';' before 'WiFi'"),
          "an error with a line and a column");
    check(problem_is(problems[1], problem_severity::error, "/libraries/Strip/src/Strip.h", 3, 10,
                     "Missing.h: No such file or directory"),
          "a fatal error reads as an error, its carriage return dropped");
    check(problem_is(problems[2], problem_severity::warning, "/sketches/Lamp/Lamp.ino", 5, 0,
                     "unused variable 'x' [-Wunused-variable]"),
          "a warning without a column");
    check(problem_is(problems[3], problem_severity::note, "/sketches/Lamp/Lamp.ino", 7, 1,
                     "in expansion of macro 'GLOW'"),
          "a note");
    check(problem_is(problems[4], problem_severity::error, "/sketches/Lamp/Lamp.ino", 9, 2,
                     "a message: with error: inside it"),
          "the first severity after a location starts the message");
    check(find_compiler_problems("").empty(), "no output names no problem");
}

int main() {
    check_problems();

    if (g_failures != 0) {
        printf("compiler_output: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("compiler_output: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
