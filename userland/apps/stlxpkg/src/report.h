#ifndef STLXPKG_REPORT_H
#define STLXPKG_REPORT_H

/* What a run tells its caller, sentences for a person or with --events the lines packages/README.md lists for a
 * program */

#include <stdint.h>
#include <stdio.h>

typedef enum {
    FAILURE_NETWORK,
    FAILURE_TIMEOUT,
    FAILURE_MEMORY,
    FAILURE_CONFLICT,
    FAILURE_VERIFY,
    FAILURE_LOCK,
    FAILURE_MISSING,
    FAILURE_IO,
} failure_kind;

/**
 * @brief Prints facts as lines to `out` from now on instead of sentences.
 */
void report_use_events(FILE* out);

/**
 * @brief Reports why the run or its current package failed. A person reads the sentence at once on stderr, and with
 * --events the first failure is kept for report_failed.
 * @return -1, for the caller to return.
 */
int fail(failure_kind kind, const char* format, ...) __attribute__((format(printf, 2, 3)));

/**
 * @brief The kind of an errno, memory for ENOMEM and io for the rest.
 */
failure_kind failure_of_errno(int error);

/**
 * @brief Reports a failed stlxhttp call by its STLXHTTP_ERR_* code and message, as timeout or memory where the code
 * says so and as network otherwise.
 * @return -1, for the caller to return.
 */
int fail_http(int32_t error, const char* message);

/**
 * @brief Prints the kept failure as `failed <name> <kind> <reason>` and forgets it, with --events only.
 */
void report_failed(const char* name);

/**
 * @brief Prints a sentence for a person on stdout, without --events only.
 */
void report_say(const char* format, ...) __attribute__((format(printf, 1, 2)));

void report_record(const char* name, const char* version);
void report_package(const char* name, const char* version, const char* state);
void report_start(const char* name, const char* version, int64_t length);

/**
 * @brief Prints a progress line unless one went out less than 100 ms before `now_ns`, the one at `length` always
 * going out.
 */
void report_progress(const char* name, int64_t done, int64_t length, uint64_t now_ns);

void report_commit(const char* name);
void report_installed(const char* name, const char* version, uint64_t size);
void report_removed(const char* name, const char* version);

#endif /* STLXPKG_REPORT_H */
