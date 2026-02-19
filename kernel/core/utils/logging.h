#ifndef STELLUX_CORE_UTILS_LOGGING_H
#define STELLUX_CORE_UTILS_LOGGING_H

#include "core/types.h"

// Default log level if not defined via defconfig
#ifndef LOG_LEVEL
#define LOG_LEVEL 0
#endif

namespace log {

enum class level : uint8_t {
    debug = 0,
    info  = 1,
    warn  = 2,
    error = 3,
    fatal = 4,
    none  = 5
};

// Backend interface for late-stage kernel ring buffer support
struct backend {
    void (*write)(const char* data, size_t len);
};

/**
 * @brief Set custom backend (nullptr = use default serial backend).
 */
void set_backend(const backend* be);

/**
 * @brief Log a debug message.
 */
void debug(const char* fmt, ...);

/**
 * @brief Log an info message.
 */
void info(const char* fmt, ...);

/**
 * @brief Log a warning message.
 */
void warn(const char* fmt, ...);

/**
 * @brief Log an error message.
 */
void error(const char* fmt, ...);

/**
 * @brief Log a fatal error and halt the system.
 */
[[noreturn]] void fatal(const char* fmt, ...);

} // namespace log

// Compile-time log level filtering macros
// These allow complete elimination of log calls below the configured level

#if LOG_LEVEL > 0
#define log_debug(...) ((void)0)
#else
#define log_debug(...) log::debug(__VA_ARGS__)
#endif

#if LOG_LEVEL > 1
#define log_info(...) ((void)0)
#else
#define log_info(...) log::info(__VA_ARGS__)
#endif

#if LOG_LEVEL > 2
#define log_warn(...) ((void)0)
#else
#define log_warn(...) log::warn(__VA_ARGS__)
#endif

#if LOG_LEVEL > 3
#define log_error(...) ((void)0)
#else
#define log_error(...) log::error(__VA_ARGS__)
#endif

#define log_fatal(...) log::fatal(__VA_ARGS__)

#endif // STELLUX_CORE_UTILS_LOGGING_H
