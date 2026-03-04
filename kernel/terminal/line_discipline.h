#ifndef STELLUX_TERMINAL_LINE_DISCIPLINE_H
#define STELLUX_TERMINAL_LINE_DISCIPLINE_H

#include "common/types.h"
#include "sync/spinlock.h"

struct ring_buffer;

namespace terminal {

constexpr size_t LD_LINE_BUF_MAX = 1023;
constexpr uint32_t LD_MODE_RAW = 1;

struct echo_target {
    void (*write)(void* ctx, const uint8_t* buf, size_t len);
    void* ctx;
};

struct line_discipline {
    uint32_t mode;
    char line_buf[LD_LINE_BUF_MAX + 1];
    size_t line_len;
    char prev_char;
    sync::spinlock lock;
};

/**
 * @brief Initialize a line discipline to cooked mode with empty state.
 */
void ld_init(line_discipline* ld);

/**
 * @brief Process a single input byte through the line discipline.
 * Acquires and releases ld->lock per call. Safe from ISR context.
 * @param ld Line discipline state.
 * @param sink Ring buffer where processed input is delivered.
 * @param echo Where echo bytes are sent (cooked mode only).
 * @param c The input byte.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ld_input(line_discipline* ld, ring_buffer* sink,
                                 const echo_target* echo, char c);

/**
 * @brief Process a buffer of input bytes through the line discipline.
 * Acquires ld->lock once for the entire buffer. More efficient than
 * per-byte ld_input for process-context callers (PTY master write).
 * Echo and ring_buffer_write are called while holding ld->lock
 * and must be nonblocking.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ld_input_buf(line_discipline* ld, ring_buffer* sink,
                                     const echo_target* echo,
                                     const char* buf, size_t len);

/**
 * @brief Switch between raw and cooked mode. Resets line buffer.
 * Elevates internally for the spinlock critical section.
 * @param cmd STLX_TCSETS_RAW or STLX_TCSETS_COOKED.
 * @return OK on success, ERR on invalid cmd.
 */
int32_t ld_set_mode(line_discipline* ld, uint32_t cmd);

} // namespace terminal

#endif // STELLUX_TERMINAL_LINE_DISCIPLINE_H
