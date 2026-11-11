/* What a paste sends the program: line endings as Enter's carriage return, no control characters that could act
 * as keys, and the bracketed paste markers exactly when the program turned the mode on */
#include "paste.h"
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

static int encodes(const char *text, size_t len, int bracketed, const char *expected, size_t expected_len) {
    char out[256];
    size_t n = paste_encode(text, len, bracketed, out);
    return n == expected_len && memcmp(out, expected, n) == 0;
}

#define ENCODES(text, bracketed, expected) \
    encodes(text, sizeof(text) - 1, bracketed, expected, sizeof(expected) - 1)

static void check_line_endings(void) {
    check(ENCODES("a\nb\n", 0, "a\rb\r"), "a newline becomes a carriage return");
    check(ENCODES("a\r\nb\r\n", 0, "a\rb\r"), "a carriage return and newline become one carriage return");
    check(ENCODES("a\rb", 0, "a\rb"), "a lone carriage return stays");
    check(ENCODES("a\n\nb", 0, "a\r\rb"), "an empty line stays");
}

static void check_controls(void) {
    check(ENCODES("a\x03" "b\x04" "c\x7f" "d\x1b" "e", 0, "abcde"), "control characters are dropped");
    check(ENCODES("a\tb", 0, "a\tb"), "a tab stays");
    check(ENCODES("caf\xc3\xa9", 0, "caf\xc3\xa9"), "UTF-8 passes through");
}

static void check_brackets(void) {
    check(ENCODES("ls\n", 1, "\x1b[200~ls\r\x1b[201~"), "bracketed paste puts the markers around the text");
    check(ENCODES("a\x1b[201~b", 1, "\x1b[200~a[201~b\x1b[201~"), "a pasted end marker cannot end the paste early");

    term_init(&g_term, 4, 20);
    check(!g_term.bracketed_paste, "bracketed paste starts off");

    term_feed(&g_term, "\x1b[?2004h", 8);
    check(g_term.bracketed_paste, "DECSET 2004 turns bracketed paste on");

    term_feed(&g_term, "\x1b[?2004l", 8);
    check(!g_term.bracketed_paste, "DECRST 2004 turns it off");

    term_feed(&g_term, "\x1b[?2004h\x1b" "c", 10);
    check(!g_term.bracketed_paste, "a reset turns it off");
}

int main(void) {
    check_line_endings();
    check_controls();
    check_brackets();

    if (g_failures != 0) {
        printf("paste: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("paste: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
