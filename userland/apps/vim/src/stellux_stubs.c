/*
 * stellux_stubs.c - Stub functions for Vim on Stellux OS
 *
 * Provides missing POSIX/system functions that Vim references but
 * Stellux doesn't implement. These stubs allow Vim to compile and
 * run for basic editing without shell commands or advanced features.
 */

#include <stddef.h>
#include <string.h>

/* ===== vim9script stubs ===== */
/* These are referenced by ex_docmd.c and other core files but only
   meaningful for Vim9 script support which we don't need. */

int in_vim9script(void) { return 0; }

void ex_vim9script(void *eap) {
    (void)eap;
}

/* vim9_comment_start is referenced by various parsing code */
char *vim9_comment_start(char *p) {
    (void)p;
    return NULL;
}

/* ===== term_set_winsize stub ===== */
/* Called from os_unix.c mch_set_shellsize() when T_CWS is set.
   Without ncurses/termcap this function isn't compiled in term.c.
   Stellux handles terminal size via TIOCSWINSZ ioctl instead. */
void term_set_winsize(int height, int width) {
    (void)height;
    (void)width;
}
