#ifndef XHCI_LOG_H
#define XHCI_LOG_H
#include <serial/serial.h>

// Possible log levels
#define XHCI_LOG_LEVEL_VERBOSE  1
#define XHCI_LOG_LEVEL_DBG      2
#define XHCI_LOG_LEVEL_WARN     3
#define XHCI_LOG_LEVEL_ERROR    4
#define XHCI_LOG_LEVEL_NONE     5

// Current log verbosity level
// #define XHCI_LOG_LEVEL XHCI_LOG_LEVEL_VERBOSE
#define XHCI_LOG_LEVEL XHCI_LOG_LEVEL_DBG

// Buffer size for log messages
constexpr size_t LOG_BUFFER_SIZE = 256;

template <typename... Args>
void xhci_log_internal(int level, const char* prefix, const char* format, Args... args) {
    if (level < XHCI_LOG_LEVEL) {
        return;
    }

    // Create the message buffer
    char buffer[LOG_BUFFER_SIZE] = { 0 };

    // Format the message with the prefix
    int len = sprintf(buffer, LOG_BUFFER_SIZE, "[XHCI]%s: ", prefix);
    if (len < 0) {
        return;
    }

    // Append the actual formatted log message
    int msg_len = sprintf(buffer + len, LOG_BUFFER_SIZE - len, format, args...);
    if (msg_len < 0) {
        return;
    }

    // Send the complete log message
    serial::printf(buffer);
}

template <typename... Args>
void xhci_log(const char* format, Args... args) {
    xhci_log_internal(XHCI_LOG_LEVEL, "", format, args...);
}

template <typename... Args>
void xhci_logv(const char* format, Args... args) {
    xhci_log_internal(XHCI_LOG_LEVEL_VERBOSE, "", format, args...);
}

template <typename... Args>
void xhci_warn(const char* format, Args... args) {
    xhci_log_internal(XHCI_LOG_LEVEL_WARN, " WARN", format, args...);
}

template <typename... Args>
void xhci_error(const char* format, Args... args) {
    xhci_log_internal(XHCI_LOG_LEVEL_ERROR, " ERROR", format, args...);
}

#endif // XHCI_LOG_H
