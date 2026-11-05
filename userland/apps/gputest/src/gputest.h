#ifndef GPUTEST_H
#define GPUTEST_H

#include <stdbool.h>
#include <stdint.h>

// Prints a PASS or FAIL line for one check and counts it
void check(const char* name, bool cond);

uint64_t monotonic_ns(void);

// The window protocol's checks, which speak it over sockets of their own and need no GPU
void run_proto_checks(void);

#endif
