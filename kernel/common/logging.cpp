#include "logging.h"
#include "varargs.h"
#include "string.h"
#include "io/serial.h"
#include "hw/cpu.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"

namespace log {

// Current backend (nullptr = use serial directly)
static const backend* current_backend = nullptr;

__PRIVILEGED_DATA static sync::spinlock g_log_lock = sync::SPINLOCK_INIT;

// Level prefixes
static const char* level_prefixes[] = {
    "[DEBUG] ",
    "[INFO]  ",
    "[WARN]  ",
    "[ERROR] ",
    "[FATAL] "
};

void set_backend(const backend* be) {
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_log_lock);
        current_backend = be;
    });
}

// Output a string to the current backend
static void output(const char* str, size_t len) {
    if (current_backend && current_backend->write) {
        current_backend->write(str, len);
    } else {
        serial::write(str, len);
    }
}

static void output_char(char c) {
    output(&c, 1);
}

static void output_str(const char* s) {
    output(s, string::strlen(s));
}

// Number to string conversion buffer
// NOTE: Not thread-safe - will need per-CPU buffers or stack allocation for SMP
static char num_buffer[65]; // Enough for 64-bit binary + null

// Convert unsigned integer to string with given base
static const char* utoa(uint64_t value, int base, bool uppercase, int width, char pad) {
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char* digits = uppercase ? digits_upper : digits_lower;
    
    char* p = num_buffer + sizeof(num_buffer) - 1;
    *p = '\0';
    
    int digit_count = 0;
    do {
        *--p = digits[value % base];
        value /= base;
        digit_count++;
    } while (value != 0);
    
    // Add padding if needed
    while (digit_count < width) {
        *--p = pad;
        digit_count++;
    }
    
    return p;
}

// Convert signed integer to string
static const char* itoa(int64_t value, int width, char pad) {
    bool negative = value < 0;
    uint64_t abs_value = negative ? -static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    
    const char* str = utoa(abs_value, 10, false, negative ? (width > 0 ? width - 1 : 0) : width, pad);
    
    if (negative) {
        // Find start of string and prepend minus
        char* p = const_cast<char*>(str) - 1;
        *p = '-';
        return p;
    }
    
    return str;
}

// Core format engine -- processes format string and outputs via output()/output_char()
static void vformat(const char* fmt, va_list args) {
    while (*fmt) {
        if (*fmt != '%') {
            output_char(*fmt);
            fmt++;
            continue;
        }

        fmt++;
        if (*fmt == '\0') break;

        int width = 0;
        char pad = ' ';

        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }

        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        bool is_long = false;
        bool is_size = false;

        if (*fmt == 'l') {
            is_long = true;
            fmt++;
        } else if (*fmt == 'z') {
            is_size = true;
            fmt++;
        }
        
        // Parse conversion specifier
        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t val;
                if (is_long) {
                    val = va_arg(args, int64_t);
                } else if (is_size) {
                    val = static_cast<int64_t>(va_arg(args, size_t));
                } else {
                    val = va_arg(args, int);
                }
                output_str(itoa(val, width, pad));
                break;
            }

            case 'u': {
                uint64_t val;
                if (is_long) {
                    val = va_arg(args, uint64_t);
                } else if (is_size) {
                    val = va_arg(args, size_t);
                } else {
                    val = va_arg(args, unsigned int);
                }
                output_str(utoa(val, 10, false, width, pad));
                break;
            }

            case 'x':
            case 'X': {
                uint64_t val;
                if (is_long) {
                    val = va_arg(args, uint64_t);
                } else if (is_size) {
                    val = va_arg(args, size_t);
                } else {
                    val = va_arg(args, unsigned int);
                }
                output_str(utoa(val, 16, *fmt == 'X', width, pad));
                break;
            }

            case 'p': {
                uintptr_t val = reinterpret_cast<uintptr_t>(va_arg(args, void*));
                output_str("0x");
                output_str(utoa(val, 16, false, sizeof(void*) * 2, '0'));
                break;
            }

            case 's': {
                const char* s = va_arg(args, const char*);
                if (s == nullptr) {
                    s = "(null)";
                }
                size_t len = string::strlen(s);
                if (precision >= 0 && static_cast<size_t>(precision) < len) {
                    len = static_cast<size_t>(precision);
                }
                // Pad with spaces if width specified
                while (width > 0 && static_cast<int>(len) < width) {
                    output_char(' ');
                    width--;
                }
                output(s, len);
                break;
            }

            case 'c': {
                char c = static_cast<char>(va_arg(args, int));
                output_char(c);
                break;
            }

            case 'b': {
                uint64_t val;
                if (is_long) {
                    val = va_arg(args, uint64_t);
                } else {
                    val = va_arg(args, unsigned int);
                }
                output_str(utoa(val, 2, false, width, pad));
                break;
            }

            case '%':
                output_char('%');
                break;

            default:
                output_char('%');
                output_char(*fmt);
                break;
        }

        fmt++;
    }
}

static void vlog(level lvl, const char* fmt, va_list args) {
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_log_lock);
        output_str(level_prefixes[static_cast<int>(lvl)]);
        vformat(fmt, args);
        output_str("\r\n");
    });
}

void debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(level::debug, fmt, args);
    va_end(args);
}

void info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(level::info, fmt, args);
    va_end(args);
}

void warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(level::warn, fmt, args);
    va_end(args);
}

void error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(level::error, fmt, args);
    va_end(args);
}

// Emergency serial output for fatal() — bypasses log_lock and num_buffer.
// Uses stack-local buffer to avoid races with concurrent formatters.
static void fatal_serial_char(char c) {
    serial::write(&c, 1);
}

static void fatal_serial_str(const char* s) {
    serial::write(s, string::strlen(s));
}

static void fatal_serial_hex(uint64_t val, int width) {
    static const char hex[] = "0123456789abcdef";
    if (width > 16) width = 16;
    char buf[17];
    char* p = buf + 16;
    *p = '\0';
    int digits = 0;
    do {
        *--p = hex[val & 0xf];
        val >>= 4;
        digits++;
    } while (val != 0);
    while (digits < width) { *--p = '0'; digits++; }
    fatal_serial_str(p);
}

static void fatal_vformat(const char* fmt, va_list args) {
    char local_buf[65];
    while (*fmt) {
        if (*fmt != '%') { fatal_serial_char(*fmt++); continue; }
        fmt++;
        if (*fmt == '\0') break;

        int width = 0;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        bool is_long = false;
        if (*fmt == 'l') { is_long = true; fmt++; }
        else if (*fmt == 'z') { is_long = true; fmt++; }

        switch (*fmt) {
            case 's': {
                const char* s = va_arg(args, const char*);
                fatal_serial_str(s ? s : "(null)");
                break;
            }
            case 'u': case 'd': case 'i': {
                uint64_t val;
                if (*fmt == 'd' || *fmt == 'i') {
                    int64_t sv = is_long ? va_arg(args, int64_t) : va_arg(args, int);
                    if (sv < 0) { fatal_serial_char('-'); sv = -sv; }
                    val = static_cast<uint64_t>(sv);
                } else {
                    val = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                }
                char* p = local_buf + sizeof(local_buf) - 1;
                *p = '\0';
                int digits = 0;
                do { *--p = '0' + static_cast<char>(val % 10); val /= 10; digits++; } while (val);
                while (digits < width) { *--p = pad; digits++; }
                fatal_serial_str(p);
                break;
            }
            case 'x': case 'X': {
                uint64_t val = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                fatal_serial_hex(val, width);
                break;
            }
            case 'p': {
                uintptr_t val = reinterpret_cast<uintptr_t>(va_arg(args, void*));
                fatal_serial_str("0x");
                fatal_serial_hex(val, sizeof(void*) * 2);
                break;
            }
            case 'c': fatal_serial_char(static_cast<char>(va_arg(args, int))); break;
            case '%': fatal_serial_char('%'); break;
            default: fatal_serial_char('%'); fatal_serial_char(*fmt); break;
        }
        fmt++;
    }
}

[[noreturn]] void fatal(const char* fmt, ...) {
    if (!dynpriv::is_elevated()) {
        dynpriv::elevate();
    }
    cpu::irq_disable();

    fatal_serial_str("[FATAL] ");
    va_list args;
    va_start(args, fmt);
    fatal_vformat(fmt, args);
    va_end(args);
    fatal_serial_str("\r\n");

    for (;;) {
        cpu::halt();
    }
}

void raw(const char* fmt, ...) {
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_log_lock);
        va_list args;
        va_start(args, fmt);
        vformat(fmt, args);
        output_str("\r\n");
        va_end(args);
    });
}

void vraw(const char* fmt, va_list args) {
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_log_lock);
        vformat(fmt, args);
        output_str("\r\n");
    });
}

void panic_write(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fatal_vformat(fmt, args);
    va_end(args);
    fatal_serial_str("\r\n");
}

} // namespace log
