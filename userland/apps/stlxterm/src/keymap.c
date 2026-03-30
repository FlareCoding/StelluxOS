#include "keymap.h"
#include <stlx/input.h>

#define SHIFT_MASK (STLX_INPUT_MOD_LSHIFT | STLX_INPUT_MOD_RSHIFT)
#define CTRL_MASK  (STLX_INPUT_MOD_LCTRL  | STLX_INPUT_MOD_RCTRL)

static const char DIGIT_UNSHIFTED[] = "1234567890";
static const char DIGIT_SHIFTED[]   = "!@#$%^&*()";

static const char PUNCT_UNSHIFTED[] = {
    '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/'
};
static const char PUNCT_SHIFTED[] = {
    '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?'
};

static int emit(char *out, int out_size, const char *seq, int len) {
    if (len > out_size) return 0;
    for (int i = 0; i < len; i++) out[i] = seq[i];
    return len;
}

int keymap_translate(uint16_t usage, uint8_t modifiers,
                     char *out, int out_size) {
    if (!out || out_size < 1) return 0;

    int shift = (modifiers & SHIFT_MASK) != 0;
    int ctrl  = (modifiers & CTRL_MASK) != 0;

    if (usage >= 0xE0 && usage <= 0xE7) return 0;

    if (usage >= 0x04 && usage <= 0x1D) {
        char ch = (char)('a' + (usage - 0x04));
        if (ctrl) {
            out[0] = ch & 0x1F;
            return 1;
        }
        out[0] = shift ? (char)(ch - 32) : ch;
        return 1;
    }

    if (usage >= 0x1E && usage <= 0x27) {
        int idx = usage - 0x1E;
        out[0] = shift ? DIGIT_SHIFTED[idx] : DIGIT_UNSHIFTED[idx];
        return 1;
    }

    if (usage >= 0x2D && usage <= 0x38) {
        int idx = usage - 0x2D;
        if (idx < 0 || idx >= (int)sizeof(PUNCT_UNSHIFTED)) return 0;
        char ch = shift ? PUNCT_SHIFTED[idx] : PUNCT_UNSHIFTED[idx];
        if (ch == 0) return 0;
        out[0] = ch;
        return 1;
    }

    switch (usage) {
    case 0x28: out[0] = '\r';   return 1;
    case 0x29: out[0] = '\x1b'; return 1;
    case 0x2A: out[0] = '\x7f'; return 1;
    case 0x2B: out[0] = '\t';   return 1;
    case 0x2C: out[0] = ' ';    return 1;

    /* Arrow keys */
    case 0x4F: return emit(out, out_size, "\x1b[C",  3);  /* Right */
    case 0x50: return emit(out, out_size, "\x1b[D",  3);  /* Left */
    case 0x51: return emit(out, out_size, "\x1b[B",  3);  /* Down */
    case 0x52: return emit(out, out_size, "\x1b[A",  3);  /* Up */

    /* Navigation keys */
    case 0x4A: return emit(out, out_size, "\x1b[H",  3);  /* Home */
    case 0x4D: return emit(out, out_size, "\x1b[F",  3);  /* End */
    case 0x49: return emit(out, out_size, "\x1b[2~", 4);  /* Insert */
    case 0x4C: return emit(out, out_size, "\x1b[3~", 4);  /* Delete */
    case 0x4B: return emit(out, out_size, "\x1b[5~", 4);  /* Page Up */
    case 0x4E: return emit(out, out_size, "\x1b[6~", 4);  /* Page Down */

    /* Function keys F1-F12 */
    case 0x3A: return emit(out, out_size, "\x1bOP",   3);  /* F1 */
    case 0x3B: return emit(out, out_size, "\x1bOQ",   3);  /* F2 */
    case 0x3C: return emit(out, out_size, "\x1bOR",   3);  /* F3 */
    case 0x3D: return emit(out, out_size, "\x1bOS",   3);  /* F4 */
    case 0x3E: return emit(out, out_size, "\x1b[15~", 5);  /* F5 */
    case 0x3F: return emit(out, out_size, "\x1b[17~", 5);  /* F6 */
    case 0x40: return emit(out, out_size, "\x1b[18~", 5);  /* F7 */
    case 0x41: return emit(out, out_size, "\x1b[19~", 5);  /* F8 */
    case 0x42: return emit(out, out_size, "\x1b[20~", 5);  /* F9 */
    case 0x43: return emit(out, out_size, "\x1b[21~", 5);  /* F10 */
    case 0x44: return emit(out, out_size, "\x1b[23~", 5);  /* F11 */
    case 0x45: return emit(out, out_size, "\x1b[24~", 5);  /* F12 */
    }

    return 0;
}
