#define _POSIX_C_SOURCE 200809L

#include "report.h"
#include "cancel.h"

#include <stlxhttp/http.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#define REASON_MAX           (2 * PATH_MAX + 512)
#define PROGRESS_INTERVAL_NS 100000000ull

static const char* const FAILURE_KINDS[] = {
    [FAILURE_NETWORK]   = "network",
    [FAILURE_TIMEOUT]   = "timeout",
    [FAILURE_MEMORY]    = "memory",
    [FAILURE_CONFLICT]  = "conflict",
    [FAILURE_VERIFY]    = "verify",
    [FAILURE_LOCK]      = "lock",
    [FAILURE_MISSING]   = "missing",
    [FAILURE_CANCELLED] = "cancelled",
    [FAILURE_IO]        = "io",
};

static FILE*        g_events;
static bool         g_has_failure;
static failure_kind g_failure_kind;
static char         g_failure_reason[REASON_MAX];
static bool         g_progress_sent;
static uint64_t     g_progress_sent_ns;

void report_use_events(FILE* out) {
    g_events = out;
}

/* Each line goes out at once, since a program reads every fact as it happens */
static __attribute__((format(printf, 1, 2))) void emit(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(g_events, format, args);
    va_end(args);

    fflush(g_events);
}

int fail(failure_kind kind, const char* format, ...) {
    char reason[REASON_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(reason, sizeof(reason), format, args);
    va_end(args);

    if (!g_events) {
        fprintf(stderr, "stlxpkg: %s\n", reason);
        return -1;
    }

    if (!g_has_failure) {
        g_has_failure = true;
        g_failure_kind = kind;
        memcpy(g_failure_reason, reason, sizeof(reason));
    }

    return -1;
}

failure_kind failure_of_errno(int error) {
    return error == ENOMEM ? FAILURE_MEMORY : FAILURE_IO;
}

int fail_http(int32_t error, const char* message) {
    if (cancel_check() != 0) {
        return -1;
    }

    failure_kind kind = FAILURE_NETWORK;
    if (error == STLXHTTP_ERR_TIMEOUT) {
        kind = FAILURE_TIMEOUT;
    } else if (error == STLXHTTP_ERR_NO_MEMORY) {
        kind = FAILURE_MEMORY;
    }

    return fail(kind, "%s", message);
}

void report_failed(const char* name) {
    if (!g_events) {
        return;
    }

    emit("failed %s %s %s\n", name, FAILURE_KINDS[g_failure_kind], g_failure_reason);
    g_has_failure = false;
}

void report_say(const char* format, ...) {
    if (g_events) {
        return;
    }

    char sentence[REASON_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(sentence, sizeof(sentence), format, args);
    va_end(args);

    printf("stlxpkg: %s\n", sentence);
}

void report_record(const char* name, const char* version) {
    if (g_events) {
        emit("record %s %s\n", name, version);
    }
}

void report_package(const char* name, const char* version, const char* state) {
    if (g_events) {
        emit("package %s %s %s\n", name, version, state);
    }
}

void report_start(const char* name, const char* version, int64_t length) {
    g_progress_sent = false;
    if (g_events) {
        emit("start %s %s %lld\n", name, version, (long long)length);
    }
}

void report_progress(const char* name, int64_t done, int64_t length, uint64_t now_ns) {
    bool is_last = done == length;
    bool is_due = !g_progress_sent || now_ns - g_progress_sent_ns >= PROGRESS_INTERVAL_NS;
    if (!g_events || (!is_last && !is_due)) {
        return;
    }

    g_progress_sent = true;
    g_progress_sent_ns = now_ns;
    emit("progress %s %lld %lld\n", name, (long long)done, (long long)length);
}

void report_commit(const char* name) {
    if (g_events) {
        emit("commit %s\n", name);
    }
}

void report_installed(const char* name, const char* version, uint64_t size) {
    if (g_events) {
        emit("installed %s %s %llu\n", name, version, (unsigned long long)size);
    }
}

void report_removed(const char* name, const char* version) {
    if (g_events) {
        emit("removed %s %s\n", name, version);
    }
}
