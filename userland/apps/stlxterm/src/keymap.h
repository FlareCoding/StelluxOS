#ifndef STLXTERM_KEYMAP_H
#define STLXTERM_KEYMAP_H

#include <stdint.h>

// Encodes keys that produce escape sequences rather than characters
// (arrows, navigation, function keys, escape, backspace). Printable
// input arrives already translated by the display manager.
int keymap_translate(uint16_t usage, int app_cursor,
                     char *out, int out_size);

#endif // STLXTERM_KEYMAP_H
