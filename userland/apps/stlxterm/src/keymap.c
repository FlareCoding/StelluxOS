#include "keymap.h"

static int emit(char *out, int out_size, const char *seq, int len) {
    if (len > out_size) return 0;
    for (int i = 0; i < len; i++) out[i] = seq[i];
    return len;
}

int keymap_translate(uint16_t usage, int app_cursor,
                     char *out, int out_size) {
    if (!out || out_size < 1) return 0;

    switch (usage) {
    case 0x29: out[0] = '\x1b'; return 1;  /* Escape */
    case 0x2A: out[0] = '\b';   return 1;  /* Backspace */

    /* Arrows follow DECCKM: CSI form normally, SS3 form in application mode */
    case 0x4F: return emit(out, out_size, app_cursor ? "\x1bOC" : "\x1b[C", 3);
    case 0x50: return emit(out, out_size, app_cursor ? "\x1bOD" : "\x1b[D", 3);
    case 0x51: return emit(out, out_size, app_cursor ? "\x1bOB" : "\x1b[B", 3);
    case 0x52: return emit(out, out_size, app_cursor ? "\x1bOA" : "\x1b[A", 3);
    case 0x4A: return emit(out, out_size, app_cursor ? "\x1bOH" : "\x1b[H", 3);
    case 0x4D: return emit(out, out_size, app_cursor ? "\x1bOF" : "\x1b[F", 3);
    case 0x4C: return emit(out, out_size, "\x1b[3~", 4);
    case 0x49: return emit(out, out_size, "\x1b[2~", 4);  /* Insert */
    case 0x4B: return emit(out, out_size, "\x1b[5~", 4);  /* Page Up */
    case 0x4E: return emit(out, out_size, "\x1b[6~", 4);  /* Page Down */

    /* Function keys F1-F12, xterm sequences */
    case 0x3A: return emit(out, out_size, "\x1bOP",   3);
    case 0x3B: return emit(out, out_size, "\x1bOQ",   3);
    case 0x3C: return emit(out, out_size, "\x1bOR",   3);
    case 0x3D: return emit(out, out_size, "\x1bOS",   3);
    case 0x3E: return emit(out, out_size, "\x1b[15~", 5);
    case 0x3F: return emit(out, out_size, "\x1b[17~", 5);
    case 0x40: return emit(out, out_size, "\x1b[18~", 5);
    case 0x41: return emit(out, out_size, "\x1b[19~", 5);
    case 0x42: return emit(out, out_size, "\x1b[20~", 5);
    case 0x43: return emit(out, out_size, "\x1b[21~", 5);
    case 0x44: return emit(out, out_size, "\x1b[23~", 5);
    case 0x45: return emit(out, out_size, "\x1b[24~", 5);
    }

    return 0;
}
