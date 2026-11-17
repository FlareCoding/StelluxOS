#define _POSIX_C_SOURCE 200809L

#include "options.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVENTS_FLAG  "--events"
#define PORT_PREFIX  PORT_DIRECTORY "/"
#define BREAK_MS_MAX 10000
#define VALUES_MAX   64

#define LENGTH_OF(array) (sizeof(array) / sizeof((array)[0]))

typedef enum {
    OPTION_BAUD,
    OPTION_DATA_BITS,
    OPTION_PARITY,
    OPTION_STOP_BITS,
    OPTION_FLOW,
    OPTION_HUPCL,
    OPTION_DTR,
    OPTION_RTS,
    OPTION_PULSE,
    OPTION_BREAK,
    OPTION_ECHO,
    OPTION_EOL,
    OPTION_XONXOFF,
} option_id;

typedef enum {
    SWITCH_ON,
    SWITCH_OFF,
} switch_word;

// An option takes one of its words, each giving the value of its index, or a number in its range when it has none
typedef struct {
    const char*        name;
    const char* const* words;
    size_t             word_count;
    uint32_t           min;
    uint32_t           max;
} option_spec;

static const char* const PARITY_WORDS[] = {
    [PARITY_NONE]  = "none",
    [PARITY_EVEN]  = "even",
    [PARITY_ODD]   = "odd",
    [PARITY_MARK]  = "mark",
    [PARITY_SPACE] = "space",
};

static const char* const FLOW_WORDS[] = {
    [FLOW_NONE]   = "none",
    [FLOW_RTSCTS] = "rtscts",
};

static const char* const HUPCL_WORDS[] = {
    [HUPCL_ON]  = "on",
    [HUPCL_OFF] = "off",
};

static const char* const LEVEL_WORDS[] = {
    [LEVEL_RAISED]  = "on",
    [LEVEL_LOWERED] = "off",
};

static const char* const PULSE_WORDS[] = {
    [PULSE_NONE] = "none",
    [PULSE_DTR]  = "dtr",
    [PULSE_RTS]  = "rts",
};

static const char* const SWITCH_WORDS[] = {
    [SWITCH_ON]  = "on",
    [SWITCH_OFF] = "off",
};

static const char* const EOL_WORDS[] = {
    [EOL_NONE] = "none",
    [EOL_CR]   = "cr",
    [EOL_LF]   = "lf",
    [EOL_CRLF] = "crlf",
};

static const option_spec OPTIONS[] = {
    [OPTION_BAUD]      = { "--baud", NULL, 0, 1, UINT32_MAX },
    [OPTION_DATA_BITS] = { "--data-bits", NULL, 0, 5, 8 },
    [OPTION_PARITY]    = { "--parity", PARITY_WORDS, LENGTH_OF(PARITY_WORDS), 0, 0 },
    [OPTION_STOP_BITS] = { "--stop-bits", NULL, 0, 1, 2 },
    [OPTION_FLOW]      = { "--flow", FLOW_WORDS, LENGTH_OF(FLOW_WORDS), 0, 0 },
    [OPTION_HUPCL]     = { "--hupcl", HUPCL_WORDS, LENGTH_OF(HUPCL_WORDS), 0, 0 },
    [OPTION_DTR]       = { "--dtr", LEVEL_WORDS, LENGTH_OF(LEVEL_WORDS), 0, 0 },
    [OPTION_RTS]       = { "--rts", LEVEL_WORDS, LENGTH_OF(LEVEL_WORDS), 0, 0 },
    [OPTION_PULSE]     = { "--pulse", PULSE_WORDS, LENGTH_OF(PULSE_WORDS), 0, 0 },
    [OPTION_BREAK]     = { "--break", NULL, 0, 0, BREAK_MS_MAX },
    [OPTION_ECHO]      = { "--echo", SWITCH_WORDS, LENGTH_OF(SWITCH_WORDS), 0, 0 },
    [OPTION_EOL]       = { "--eol", EOL_WORDS, LENGTH_OF(EOL_WORDS), 0, 0 },
    [OPTION_XONXOFF]   = { "--xonxoff", SWITCH_WORDS, LENGTH_OF(SWITCH_WORDS), 0, 0 },
};

static const char USAGE[] =
    "Usage: serial list [--events]\n"
    "       serial watch [--events]\n"
    "       serial monitor <port> [--events] [options]\n"
    "\n"
    "<port> is a name such as cp2102-0001, or its path in /dev/serial.\n"
    "--events prints tab-separated facts instead of sentences.\n"
    "\n"
    "Each monitor option takes one value, as the next argument:\n"
    "  --baud <rate>                      speed in baud (115200)\n"
    "  --data-bits 5|6|7|8                data bits (8)\n"
    "  --parity none|even|odd|mark|space  parity (none)\n"
    "  --stop-bits 1|2                    stop bits (1)\n"
    "  --flow none|rtscts                 RTS/CTS flow control (none)\n"
    "  --hupcl on|off                     lower DTR and RTS at the last close (kept)\n"
    "  --dtr on|off                       DTR level (as the open leaves it)\n"
    "  --rts on|off                       RTS level (as the open leaves it)\n"
    "  --pulse none|dtr|rts               flip a line for 100 ms at the start (none)\n"
    "  --break <ms>                       hold a break at the start, 0 to 10000 (0)\n"
    "  --echo on|off                      show what is typed (off)\n"
    "  --eol none|cr|lf|crlf              the board's line end (none)\n"
    "  --xonxoff on|off                   XON/XOFF flow control (off)\n"
    "\n"
    "Ctrl-] quits the monitor.\n";

static __attribute__((format(printf, 3, 4))) int refuse(char* problem, size_t problem_size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(problem, problem_size, format, args);
    va_end(args);

    return -1;
}

/* The option named by the first `length` characters of `name`, or NULL */
static const option_spec* find_option(const char* name, size_t length) {
    for (size_t i = 0; i < LENGTH_OF(OPTIONS); i++) {
        if (strlen(OPTIONS[i].name) == length && strncmp(OPTIONS[i].name, name, length) == 0) {
            return &OPTIONS[i];
        }
    }

    return NULL;
}

static bool parse_value(const option_spec* option, const char* text, uint32_t* value) {
    if (option->words) {
        for (size_t i = 0; i < option->word_count; i++) {
            if (option->words[i] && strcmp(option->words[i], text) == 0) {
                *value = (uint32_t)i;
                return true;
            }
        }

        return false;
    }

    if (*text < '0' || *text > '9') {
        return false;
    }

    char* end = NULL;
    errno = 0;
    unsigned long long number = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || number < option->min || number > option->max) {
        return false;
    }

    *value = (uint32_t)number;
    return true;
}

/* Writes what an option takes, such as "on or off" or "a number from 0 to 10000" */
static void describe_values(const option_spec* option, char* text, size_t size) {
    if (!option->words) {
        snprintf(text, size, "a number from %" PRIu32 " to %" PRIu32, option->min, option->max);
        return;
    }

    size_t named = 0;
    for (size_t i = 0; i < option->word_count; i++) {
        named += option->words[i] != NULL;
    }

    size_t written = 0;
    size_t listed = 0;
    for (size_t i = 0; i < option->word_count && written < size; i++) {
        if (!option->words[i]) {
            continue;
        }

        listed++;
        const char* joint = listed == 1 ? "" : listed == named ? " or " : ", ";
        written += (size_t)snprintf(text + written, size - written, "%s%s", joint, option->words[i]);
    }
}

static void set_option(serial_args* out, option_id id, uint32_t value) {
    switch (id) {
        case OPTION_BAUD:
            out->line.baud = value;
            break;
        case OPTION_DATA_BITS:
            out->line.data_bits = (uint8_t)value;
            break;
        case OPTION_PARITY:
            out->line.parity = (parity_kind)value;
            break;
        case OPTION_STOP_BITS:
            out->line.stop_bits = (uint8_t)value;
            break;
        case OPTION_FLOW:
            out->line.flow = (flow_kind)value;
            break;
        case OPTION_HUPCL:
            out->line.hupcl = (hupcl_choice)value;
            break;
        case OPTION_DTR:
            out->levels.dtr = (line_level)value;
            break;
        case OPTION_RTS:
            out->levels.rts = (line_level)value;
            break;
        case OPTION_PULSE:
            out->pulse = (line_pulse)value;
            break;
        case OPTION_BREAK:
            out->break_ms = value;
            break;
        case OPTION_ECHO:
            out->session.echo = value == SWITCH_ON;
            break;
        case OPTION_EOL:
            out->session.eol = (eol_kind)value;
            break;
        case OPTION_XONXOFF:
            out->session.xonxoff = value == SWITCH_ON;
            break;
    }
}

static bool has_control_byte(const char* text) {
    for (const char* c = text; *c != '\0'; c++) {
        if (iscntrl((unsigned char)*c)) {
            return true;
        }
    }

    return false;
}

/* The port's name from a bare name or its path in PORT_DIRECTORY, or NULL for anything else. No port name holds a
 * control byte, which would also break the tab-separated lines that carry it */
static const char* parse_port_name(const char* given) {
    const char* name = strncmp(given, PORT_PREFIX, strlen(PORT_PREFIX)) == 0 ? given + strlen(PORT_PREFIX) : given;
    bool is_component = *name != '\0' && !strchr(name, '/') && strlen(name) <= NAME_MAX && !has_control_byte(name);
    bool is_directory = strcmp(name, ".") == 0 || strcmp(name, "..") == 0;

    return is_component && !is_directory ? name : NULL;
}

/* Takes the option at argv[*i] and its value, moving *i to the value */
static int take_option(int argc, char* const argv[], int* i, serial_args* out, char* problem, size_t problem_size) {
    const char* given = argv[*i];
    const option_spec* option = find_option(given, strlen(given));
    const char* equals = strchr(given, '=');
    if (!option && equals && find_option(given, (size_t)(equals - given))) {
        return refuse(problem, problem_size, "options take their value as the next argument, not after \"=\"");
    }

    if (!option) {
        return refuse(problem, problem_size, "\"%s\" is not an option", given);
    }

    if (*i + 1 >= argc) {
        return refuse(problem, problem_size, "%s needs a value", option->name);
    }

    const char* text = argv[++*i];
    uint32_t value = 0;
    if (!parse_value(option, text, &value)) {
        char values[VALUES_MAX];
        describe_values(option, values, sizeof(values));
        return refuse(problem, problem_size, "%s takes %s, not \"%s\"", option->name, values, text);
    }

    set_option(out, (option_id)(option - OPTIONS), value);
    return 0;
}

int options_parse(int argc, char* const argv[], serial_args* out, char* problem, size_t problem_size) {
    if (argc < 1) {
        return refuse(problem, problem_size, "the command is missing");
    }

    const char* command = argv[0];
    command_kind kind;
    if (strcmp(command, "list") == 0) {
        kind = COMMAND_LIST;
    } else if (strcmp(command, "watch") == 0) {
        kind = COMMAND_WATCH;
    } else if (strcmp(command, "monitor") == 0) {
        kind = COMMAND_MONITOR;
    } else {
        return refuse(problem, problem_size, "\"%s\" is not a command", command);
    }

    *out = (serial_args){
        .command = kind,
        .line    = line_settings_default(),
        .session = session_options_default(),
    };

    for (int i = 1; i < argc; i++) {
        const char* given = argv[i];
        if (strcmp(given, EVENTS_FLAG) == 0) {
            out->has_events = true;
            continue;
        }

        if (kind != COMMAND_MONITOR) {
            return refuse(problem, problem_size, "%s takes only %s, not \"%s\"", command, EVENTS_FLAG, given);
        }

        if (given[0] == '-') {
            if (take_option(argc, argv, &i, out, problem, problem_size) != 0) {
                return -1;
            }

            continue;
        }

        const char* name = parse_port_name(given);
        if (!name) {
            return refuse(problem, problem_size, "\"%s\" is not a port name or a path in %s", given, PORT_DIRECTORY);
        }

        if (out->port) {
            return refuse(problem, problem_size, "the monitor takes one port, not \"%s\" as well", given);
        }

        out->port = name;
    }

    if (kind == COMMAND_MONITOR && !out->port) {
        return refuse(problem, problem_size, "the port is missing");
    }

    return 0;
}

const char* options_usage(void) {
    return USAGE;
}
