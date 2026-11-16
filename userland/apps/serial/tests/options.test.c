/* options.c on the build machine: the commands, --events, and each usage error */
#define _POSIX_C_SOURCE 200809L

#include "options.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_REPORTS 8
#define PROBLEM_MAX 256
#define WORDS_MAX   256
#define ARGS_MAX    32

static uint64_t g_checks;
static uint64_t g_failures;
static char     g_problem[PROBLEM_MAX];

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

/* Parses `line`, the arguments after the program name separated by single spaces */
static int parse(const char* line, serial_args* out) {
    static char words[WORDS_MAX];
    snprintf(words, sizeof(words), "%s", line);

    char* argv[ARGS_MAX];
    int argc = 0;
    for (char* word = strtok(words, " "); word && argc < ARGS_MAX; word = strtok(NULL, " ")) {
        argv[argc++] = word;
    }

    g_problem[0] = '\0';
    return options_parse(argc, argv, out, g_problem, sizeof(g_problem));
}

static bool refuses(const char* line, const char* problem) {
    serial_args args;
    return parse(line, &args) == -1 && strcmp(g_problem, problem) == 0;
}

static void check_commands(void) {
    serial_args args;
    check(parse("list", &args) == 0 && args.command == COMMAND_LIST && !args.has_events, "list alone lists");
    check(parse("watch --events", &args) == 0 && args.command == COMMAND_WATCH && args.has_events,
          "watch takes --events");
    check(parse("list --events --events", &args) == 0 && args.has_events, "--events may repeat");
}

static void check_usage_errors(void) {
    check(refuses("", "the command is missing"), "a missing command is refused");
    check(refuses("frob", "\"frob\" is not a command"), "an unknown command is refused");
    check(refuses("--events list", "\"--events\" is not a command"), "the command comes first");
    check(refuses("list cp2102-0001", "list takes only --events, not \"cp2102-0001\""), "list takes no port");
    check(refuses("watch --events=1", "watch takes only --events, not \"--events=1\""), "a flag takes no value");
    check(refuses("watch --verbose", "watch takes only --events, not \"--verbose\""), "an unknown option is refused");
}

static void check_usage(void) {
    const char* first_line = "Usage: serial list [--events]\n";
    check(strncmp(options_usage(), first_line, strlen(first_line)) == 0, "the usage starts with its first command");
}

int main(void) {
    check_commands();
    check_usage_errors();
    check_usage();

    if (g_failures != 0) {
        printf("options: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("options: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
