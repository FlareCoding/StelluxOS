#ifndef STLXTERM_KEYMAP_H
#define STLXTERM_KEYMAP_H

#include <stdint.h>

/**
 * Translate a HID usage code and modifier state into a byte sequence
 * suitable for writing to a PTY master fd.
 *
 * Returns the number of bytes written to out_buf (0 if unmapped).
 * out_buf must be at least 8 bytes.
 */
int keymap_translate(uint16_t usage, uint8_t modifiers,
                     char* out_buf, int buf_size);

#endif /* STLXTERM_KEYMAP_H */
