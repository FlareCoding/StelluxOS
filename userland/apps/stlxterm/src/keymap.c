#include "keymap.h"
#include <stlx/input.h>
#include <string.h>

/* US layout: HID usage code (0x00-0x65) -> unshifted ASCII */
static const char UNSHIFTED[256] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
    [0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
    [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
    [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'y', [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
    [0x26] = '9', [0x27] = '0',
    [0x28] = '\r',  /* Enter */
    [0x29] = '\x1b', /* Escape */
    [0x2A] = '\x7f', /* Backspace */
    [0x2B] = '\t',   /* Tab */
    [0x2C] = ' ',    /* Space */
    [0x2D] = '-',  [0x2E] = '=',  [0x2F] = '[',  [0x30] = ']',
    [0x31] = '\\', [0x33] = ';',  [0x34] = '\'', [0x35] = '`',
    [0x36] = ',',  [0x37] = '.',  [0x38] = '/',
};

/* US layout: HID usage code -> shifted ASCII */
static const char SHIFTED[256] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D',
    [0x08] = 'E', [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H',
    [0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L',
    [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P',
    [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
    [0x1C] = 'Y', [0x1D] = 'Z',
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$',
    [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*',
    [0x26] = '(', [0x27] = ')',
    [0x28] = '\r',
    [0x29] = '\x1b',
    [0x2A] = '\x7f',
    [0x2B] = '\t',
    [0x2C] = ' ',
    [0x2D] = '_',  [0x2E] = '+',  [0x2F] = '{',  [0x30] = '}',
    [0x31] = '|',  [0x33] = ':',  [0x34] = '"',  [0x35] = '~',
    [0x36] = '<',  [0x37] = '>',  [0x38] = '?',
};

int keymap_translate(uint16_t usage, uint8_t modifiers,
                     char* out_buf, int buf_size) {
    if (!out_buf || buf_size < 4) {
        return 0;
    }

    int shift = (modifiers & STLX_INPUT_MOD_LSHIFT) ||
                (modifiers & STLX_INPUT_MOD_RSHIFT);
    int ctrl  = (modifiers & STLX_INPUT_MOD_LCTRL) ||
                (modifiers & STLX_INPUT_MOD_RCTRL);

    /* Arrow keys and special keys -> ANSI escape sequences */
    switch (usage) {
    case 0x4F: /* Right arrow */
        memcpy(out_buf, "\x1b[C", 3); return 3;
    case 0x50: /* Left arrow */
        memcpy(out_buf, "\x1b[D", 3); return 3;
    case 0x51: /* Down arrow */
        memcpy(out_buf, "\x1b[B", 3); return 3;
    case 0x52: /* Up arrow */
        memcpy(out_buf, "\x1b[A", 3); return 3;
    case 0x49: /* Insert */
        memcpy(out_buf, "\x1b[2~", 4); return 4;
    case 0x4A: /* Home */
        memcpy(out_buf, "\x1b[H", 3); return 3;
    case 0x4B: /* Page Up */
        memcpy(out_buf, "\x1b[5~", 4); return 4;
    case 0x4C: /* Delete */
        memcpy(out_buf, "\x1b[3~", 4); return 4;
    case 0x4D: /* End */
        memcpy(out_buf, "\x1b[F", 3); return 3;
    case 0x4E: /* Page Down */
        memcpy(out_buf, "\x1b[6~", 4); return 4;
    }

    /* Modifier keys themselves produce no output */
    if (usage >= 0xE0 && usage <= 0xE7) {
        return 0;
    }

    /* Look up character from tables */
    if (usage > 0xFF) {
        return 0;
    }

    char ch = shift ? SHIFTED[usage] : UNSHIFTED[usage];
    if (ch == 0) {
        return 0;
    }

    /* Ctrl modifies a-z/A-Z to control codes (0x01-0x1A) */
    if (ctrl && ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 1);
    } else if (ctrl && ch >= 'A' && ch <= 'Z') {
        ch = (char)(ch - 'A' + 1);
    }

    out_buf[0] = ch;
    return 1;
}
