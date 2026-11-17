#define _DEFAULT_SOURCE

#include "terminal.h"

#include <stdbool.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved_mode;
static bool           g_is_raw;

int terminal_enter_raw(void) {
    if (!isatty(STDIN_FILENO)) {
        return 0;
    }

    if (tcgetattr(STDIN_FILENO, &g_saved_mode) != 0) {
        return -1;
    }

    struct termios raw = g_saved_mode;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return -1;
    }

    g_is_raw = true;
    return 1;
}

void terminal_restore(void) {
    if (g_is_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_mode);
        g_is_raw = false;
    }
}
