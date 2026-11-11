/* What the grid keeps for selections: which rows wrapped onto the next, carried along when rows move, saved with
 * the main screen, and the count of lines scrolled off the top */
#include "term.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_REPORTS 8

static uint64_t g_checks;
static uint64_t g_failures;
static term_state_t g_term;

static void check(int holds, const char *what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static void feed(const char *output) {
    term_feed(&g_term, output, (int)strlen(output));
}

static void check_wrapping(void) {
    term_init(&g_term, 4, 5);
    feed("abcde");
    check(!g_term.wrapped[0], "a row filled to the margin has not wrapped yet");

    feed("f");
    check(g_term.wrapped[0] && !g_term.wrapped[1], "a character past the margin wraps the row");

    feed("\r\n12345\r\n");
    check(!g_term.wrapped[2], "a line ended at the margin did not wrap");

    feed("\x1b[1;1H\x1b[K");
    check(!g_term.wrapped[0], "erasing a row through its end clears its wrap");

    term_init(&g_term, 4, 5);
    feed("abcdefg\r\n\x1b[1;1H\x1b[L");
    check(!g_term.wrapped[0] && g_term.wrapped[1], "inserting a line moves the wrap down with its row");

    feed("\x1b[M");
    check(g_term.wrapped[0], "deleting a line moves the wrap up with its row");

    term_init(&g_term, 4, 5);
    feed("abcdefg\x1b[?1049h");
    check(!g_term.wrapped[0], "the alternate screen starts unwrapped");

    feed("\x1b[?1049l");
    check(g_term.wrapped[0], "leaving the alternate screen restores the main screen's wraps");
}

static void check_scrolled(void) {
    term_init(&g_term, 3, 10);
    feed("1\r\n2\r\n3");
    check(g_term.scrolled == 0, "filling the screen scrolls nothing");

    feed("\r\n4\r\n5");
    check(g_term.scrolled == 2, "each line past the bottom scrolls one off the top");

    term_init(&g_term, 4, 10);
    feed("abcdefghijk\r\n\r\n\r\n");
    check(g_term.scrolled == 1 && g_term.wrapped[0] == 0, "a scroll takes a row's wrap off with it");

    feed("\x1b[2;3r\x1b[3;1H\r\n\r\n");
    check(g_term.scrolled == 1, "scrolling inside a region scrolls nothing off the screen");

    feed("\x1b" "c");
    check(g_term.scrolled == 1, "a reset keeps the count");
}

int main(void) {
    check_wrapping();
    check_scrolled();

    if (g_failures != 0) {
        printf("term: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("term: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
