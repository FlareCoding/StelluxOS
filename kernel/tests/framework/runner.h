#ifndef STELLUX_TESTS_RUNNER_H
#define STELLUX_TESTS_RUNNER_H

#include "common/types.h"

namespace stlx_test {

/**
 * @brief Run all registered unit tests. Outputs KTAP to serial.
 * @return 0 if all tests passed, >0 = number of failures.
 */
int32_t run_all();

/**
 * @brief Write formatted output without level prefix. Appends \r\n.
 * Wraps log::raw() for KTAP output.
 */
void print(const char* fmt, ...);

} // namespace stlx_test

#endif // STELLUX_TESTS_RUNNER_H
