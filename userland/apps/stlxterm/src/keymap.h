#ifndef STLXTERM_KEYMAP_H
#define STLXTERM_KEYMAP_H

#include <stdint.h>

int keymap_translate(uint16_t usage, uint8_t modifiers,
                     char *out, int out_size);

#endif // STLXTERM_KEYMAP_H
