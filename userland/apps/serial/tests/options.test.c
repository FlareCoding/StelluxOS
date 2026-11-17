/* options.c on the build machine: the commands, the monitor's options and port, and each usage error */
#define _POSIX_C_SOURCE 200809L

#include "options.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_REPORTS 8
#define PROBLEM_MAX 256
#define WORDS_MAX   512
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

static void check_monitor_defaults(void) {
    serial_args args;
    check(parse("monitor cp2102-0001", &args) == 0 && args.command == COMMAND_MONITOR &&
              strcmp(args.port, "cp2102-0001") == 0 && !args.has_events,
          "the monitor takes a port name");

    line_settings line = args.line;
    check(line.baud == 115200 && line.data_bits == 8 && line.parity == PARITY_NONE && line.stop_bits == 1 &&
              line.flow == FLOW_NONE && line.hupcl == HUPCL_KEEP,
          "the line defaults to 115200 baud 8N1 with HUPCL kept");
    check(args.levels.dtr == LEVEL_KEEP && args.levels.rts == LEVEL_KEEP && args.pulse == PULSE_NONE &&
              args.break_ms == 0,
          "DTR and RTS stay as the open leaves them, with no pulse or break");
    check(!args.session.echo && args.session.eol == EOL_NONE && !args.session.xonxoff,
          "the session changes no byte by default");

    check(parse("monitor /dev/serial/cp2102-0001", &args) == 0 && strcmp(args.port, "cp2102-0001") == 0,
          "a path in /dev/serial names the same port");
    check(parse("monitor --events --baud 9600 cp2102-0001 --echo on", &args) == 0 &&
              strcmp(args.port, "cp2102-0001") == 0 && args.has_events && args.line.baud == 9600 && args.session.echo,
          "the port may come between options");
}

static void check_monitor_options(void) {
    serial_args args;
    check(parse("monitor p --baud 250000 --data-bits 7 --parity mark --stop-bits 2 --flow rtscts --hupcl off", &args) ==
                  0 &&
              args.line.baud == 250000 && args.line.data_bits == 7 && args.line.parity == PARITY_MARK &&
              args.line.stop_bits == 2 && args.line.flow == FLOW_RTSCTS && args.line.hupcl == HUPCL_OFF,
          "every line setting is an option");
    check(parse("monitor p --parity space --hupcl on", &args) == 0 && args.line.parity == PARITY_SPACE &&
              args.line.hupcl == HUPCL_ON,
          "space parity and --hupcl on are taken");
    check(parse("monitor p --parity even", &args) == 0 && args.line.parity == PARITY_EVEN, "even parity is taken");
    check(parse("monitor p --parity odd", &args) == 0 && args.line.parity == PARITY_ODD, "odd parity is taken");

    check(parse("monitor p --dtr off --rts on --pulse rts --break 250", &args) == 0 &&
              args.levels.dtr == LEVEL_LOWERED && args.levels.rts == LEVEL_RAISED && args.pulse == PULSE_RTS &&
              args.break_ms == 250,
          "DTR, RTS, the pulse and the break are options");
    check(parse("monitor p --pulse dtr", &args) == 0 && args.pulse == PULSE_DTR, "DTR can be pulsed");

    check(parse("monitor p --echo on --eol crlf --xonxoff on", &args) == 0 && args.session.echo &&
              args.session.eol == EOL_CRLF && args.session.xonxoff,
          "echo, the board's line end and XON and XOFF are options");
    check(parse("monitor p --eol lf", &args) == 0 && args.session.eol == EOL_LF, "--eol lf is taken");
    check(parse("monitor p --eol cr", &args) == 0 && args.session.eol == EOL_CR, "--eol cr is taken");

    check(parse("monitor p --dtr off --dtr on --pulse rts --pulse none --eol lf --eol none --echo on --echo off",
                &args) == 0 &&
              args.levels.dtr == LEVEL_RAISED && args.pulse == PULSE_NONE && args.session.eol == EOL_NONE &&
              !args.session.echo,
          "the last repeat counts, and every default can be written");
    check(parse("monitor p --break 300 --break 0 --xonxoff on --xonxoff off", &args) == 0 && args.break_ms == 0 &&
              !args.session.xonxoff,
          "--break 0 and --xonxoff off undo earlier options");
}

static void check_ranges(void) {
    serial_args args;
    check(parse("monitor p --baud 1", &args) == 0 && args.line.baud == 1, "1 baud is in range");
    check(parse("monitor p --baud 4294967295", &args) == 0 && args.line.baud == 4294967295u, "the largest speed fits");
    check(refuses("monitor p --baud 0", "--baud takes a number from 1 to 4294967295, not \"0\""), "0 baud is refused");
    check(refuses("monitor p --baud 4294967296", "--baud takes a number from 1 to 4294967295, not \"4294967296\""),
          "a speed past 32 bits is refused");
    check(refuses("monitor p --baud +9600", "--baud takes a number from 1 to 4294967295, not \"+9600\""),
          "a sign is refused");

    check(parse("monitor p --break 10000", &args) == 0 && args.break_ms == 10000, "a 10 s break is in range");
    check(refuses("monitor p --break 10001", "--break takes a number from 0 to 10000, not \"10001\""),
          "a break past 10 s is refused");
    check(refuses("monitor p --data-bits 9", "--data-bits takes a number from 5 to 8, not \"9\""),
          "9 data bits are refused");
    check(refuses("monitor p --stop-bits 3", "--stop-bits takes a number from 1 to 2, not \"3\""),
          "3 stop bits are refused");
}

static void check_usage_errors(void) {
    check(refuses("", "the command is missing"), "a missing command is refused");
    check(refuses("frob", "\"frob\" is not a command"), "an unknown command is refused");
    check(refuses("--events list", "\"--events\" is not a command"), "the command comes first");
    check(refuses("list cp2102-0001", "list takes only --events, not \"cp2102-0001\""), "list takes no port");
    check(refuses("watch --events=1", "watch takes only --events, not \"--events=1\""), "a flag takes no value");
    check(refuses("list --baud 9600", "list takes only --events, not \"--baud\""), "list takes no monitor option");

    check(refuses("monitor", "the port is missing"), "the monitor needs a port");
    check(refuses("monitor --baud 9600", "the port is missing"), "an option's value is never the port");
    check(refuses("monitor a b", "the monitor takes one port, not \"b\" as well"), "a second port is refused");
    check(refuses("monitor --echo cp2102-0001", "--echo takes on or off, not \"cp2102-0001\""),
          "a value that names a port is refused as the value");

    check(refuses("monitor p --frob 1", "\"--frob\" is not an option"), "an unknown option is refused");
    check(refuses("monitor p -h", "\"-h\" is not an option"), "options are long names only");
    check(refuses("monitor p --baud=9600", "options take their value as the next argument, not after \"=\""),
          "a value after = is refused with its own sentence");

    check(refuses("monitor p --baud", "--baud needs a value"), "a missing value is refused");
    check(refuses("monitor p --parity weird", "--parity takes none, even, odd, mark or space, not \"weird\""),
          "a word an option does not take is refused, naming the words it does");
    check(refuses("monitor p --dtr maybe", "--dtr takes on or off, not \"maybe\""), "a level is on or off");

    check(refuses("monitor /dev/ttyS0", "\"/dev/ttyS0\" is not a port name or a path in /dev/serial"),
          "a path outside /dev/serial is refused");
    check(refuses("monitor /dev/serial/", "\"/dev/serial/\" is not a port name or a path in /dev/serial"),
          "the directory alone is refused");
    check(refuses("monitor ..", "\"..\" is not a port name or a path in /dev/serial"), "a parent directory is refused");
    check(refuses("monitor /dev/serial/.", "\"/dev/serial/.\" is not a port name or a path in /dev/serial"),
          "the directory itself is refused");
    check(refuses("monitor a/b", "\"a/b\" is not a port name or a path in /dev/serial"), "a name holds no slash");

    check(refuses("monitor a\tb", "\"a\tb\" is not a port name or a path in /dev/serial"),
          "a name holds no tab, which would split the fields of its event lines");
    check(refuses("monitor a\x7f", "\"a\x7f\" is not a port name or a path in /dev/serial"), "a name holds no DEL");
}

static void check_long_names(void) {
    char line[WORDS_MAX];
    char name[300];
    memset(name, 'x', sizeof(name));

    name[255] = '\0';
    snprintf(line, sizeof(line), "monitor %s", name);
    serial_args args;
    check(parse(line, &args) == 0 && strlen(args.port) == 255, "a name of NAME_MAX bytes is taken");

    name[255] = 'x';
    name[256] = '\0';
    snprintf(line, sizeof(line), "monitor %s", name);
    check(parse(line, &args) == -1, "a longer name is refused");
}

static void check_usage(void) {
    const char* first_line = "Usage: serial list [--events]\n";
    check(strncmp(options_usage(), first_line, strlen(first_line)) == 0, "the usage starts with its first command");
    check(strstr(options_usage(), "serial monitor <port> [--events] [options]\n") != NULL,
          "the usage names the monitor");
}

int main(void) {
    check_commands();
    check_monitor_defaults();
    check_monitor_options();
    check_ranges();

    check_usage_errors();
    check_long_names();
    check_usage();

    if (g_failures != 0) {
        printf("options: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("options: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
