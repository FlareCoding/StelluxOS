#define _POSIX_C_SOURCE 200809L

#include "report.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

#define REASON_MAX 512

// What a person reads of each port, in this order
static const listing_field SHOWN_FIELDS[] = {
    FIELD_NAME, FIELD_PRODUCT, FIELD_MANUFACTURER, FIELD_USB_IDS, FIELD_SERIAL_NUMBER, FIELD_LOCATION,
};

#define SHOWN_COUNT (sizeof(SHOWN_FIELDS) / sizeof(SHOWN_FIELDS[0]))

static const char* const FAILURE_KINDS[] = {
    [FAILURE_MISSING]  = "missing",
    [FAILURE_BUSY]     = "busy",
    [FAILURE_SETTINGS] = "settings",
    [FAILURE_IO]       = "io",
};

static FILE*       g_out;
static bool        g_has_events;
static const char* g_line_end = "\n";

void report_use(FILE* out, bool has_events, bool is_raw_terminal) {
    g_out = out;
    g_has_events = has_events;
    g_line_end = is_raw_terminal ? "\r\n" : "\n";
}

/* Each line goes out at once, since a program reads every fact as it happens */
static __attribute__((format(printf, 1, 2))) void emit(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(g_out, format, args);
    va_end(args);

    fflush(g_out);
}

/* Writes a sentence for a person and ends its line the way the terminal needs */
static __attribute__((format(printf, 1, 2))) void say(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(g_out, format, args);
    va_end(args);

    emit("%s", g_line_end);
}

/* Writes the fields a person reads of `port`, padding all but the last to its column's width when `widths` is given */
static void write_fields(const listed_port* port, const int widths[]) {
    for (size_t i = 0; i + 1 < SHOWN_COUNT; i++) {
        fprintf(g_out, "%-*s  ", widths ? widths[i] : 0, port->fields[SHOWN_FIELDS[i]]);
    }

    say("%s", port->fields[SHOWN_FIELDS[SHOWN_COUNT - 1]]);
}

void report_ports(const listing* ports) {
    if (g_has_events) {
        for (size_t i = 0; i < ports->count; i++) {
            emit("port\t%s\n", ports->ports[i].line);
        }

        emit("listed\t%zu\n", ports->count);
        return;
    }

    if (ports->count == 0) {
        say("No serial ports.");
        return;
    }

    int widths[SHOWN_COUNT] = { 0 };
    for (size_t i = 0; i < ports->count; i++) {
        for (size_t column = 0; column < SHOWN_COUNT; column++) {
            int width = (int)strlen(ports->ports[i].fields[SHOWN_FIELDS[column]]);
            if (width > widths[column]) {
                widths[column] = width;
            }
        }
    }

    for (size_t i = 0; i < ports->count; i++) {
        write_fields(&ports->ports[i], widths);
    }
}

void report_added(const listed_port* port) {
    if (g_has_events) {
        emit("add\t%s\n", port->line);
        return;
    }

    fprintf(g_out, "added    ");
    write_fields(port, NULL);
}

void report_removed(const listed_port* port) {
    if (g_has_events) {
        emit("remove\t%s\n", port->fields[FIELD_NAME]);
        return;
    }

    say("removed  %s", port->fields[FIELD_NAME]);
}

void report_open(const char* name, const line_settings* settings) {
    if (g_has_events) {
        emit("open\t%s\t%" PRIu32 "\n", name, settings->baud);
        return;
    }

    char described[LINE_DESCRIPTION_MAX];
    line_settings_describe(settings, described, sizeof(described));
    say("[serial: %s/%s at %s, Ctrl-] quits]", PORT_DIRECTORY, name, described);
}

void report_gone(const char* name, int error) {
    if (g_has_events) {
        emit("gone\t%s\n", name);
    } else if (error == 0) {
        say("[serial: %s left, waiting for it to come back]", name);
    } else {
        say("[serial: %s stopped working (%s), waiting for it to be plugged in again]", name, strerror(error));
    }
}

void report_back(const char* name) {
    if (g_has_events) {
        emit("back\t%s\n", name);
        return;
    }

    say("[serial: %s is back]", name);
}

void report_dropping(const char* name) {
    if (!g_has_events) {
        say("[serial: %s is not taking typed bytes, so they are dropped]", name);
    }
}

void report_closed(const char* name) {
    if (g_has_events) {
        emit("closed\t%s\n", name);
        return;
    }

    say("[serial: closed %s]", name);
}

int report_failure(const char* subject, failure_kind kind, const char* format, ...) {
    char reason[REASON_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(reason, sizeof(reason), format, args);
    va_end(args);

    if (g_has_events) {
        emit("failed\t%s\t%s\t%s\n", subject, FAILURE_KINDS[kind], reason);
    } else {
        fprintf(stderr, "serial: %s%s", reason, g_line_end);
    }

    return 1;
}

failure_kind failure_of_errno(int error) {
    if (error == ENOENT) {
        return FAILURE_MISSING;
    }

    if (error == EBUSY || error == EWOULDBLOCK) {
        return FAILURE_BUSY;
    }

    return error == EINVAL ? FAILURE_SETTINGS : FAILURE_IO;
}
