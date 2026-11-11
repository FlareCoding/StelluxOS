/* The mouse selection over a terminal fed real output: dragging by cells, double clicks by words, triple clicks
 * by lines, Shift extending either end, the text a copy gives, and how a selection follows scrolling and change */
#include "select.h"
#include "term.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REPORTS 8

static uint64_t g_checks;
static uint64_t g_failures;
static term_state_t g_term;
static select_t g_select;

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

static void reset(int rows, int cols, const char *output) {
    term_init(&g_term, rows, cols);
    term_feed(&g_term, output, (int)strlen(output));
    memset(&g_select, 0, sizeof(g_select));
}

static void feed(const char *output) {
    term_feed(&g_term, output, (int)strlen(output));
    select_follow(&g_select, &g_term);
}

static select_point_t at(int row, int col) {
    select_point_t p = { row, col < g_term.cols ? col : g_term.cols - 1, col };
    return p;
}

static void drag(int row, int col, int to_row, int to_col) {
    select_begin(&g_select, &g_term, at(row, col), SELECT_BY_CELL);
    select_drag(&g_select, &g_term, at(to_row, to_col));
}

static int copies(const char *expected) {
    size_t len = 0;
    char *text = select_copy(&g_select, &g_term, &len);
    int matches = text && len == strlen(expected) && memcmp(text, expected, len) == 0;
    if (!matches && g_failures < MAX_REPORTS) {
        printf("  copied \"%s\", expected \"%s\"\n", text ? text : "(nothing)", expected);
    }

    free(text);
    return matches;
}

static void check_cells(void) {
    reset(4, 20, "hello world");
    drag(0, 0, 0, 5);
    check(copies("hello"), "a drag copies the cells between its edges");

    int from = -1;
    int to = -1;
    check(select_row_span(&g_select, &g_term, 0, &from, &to) && from == 0 && to == 5, "the drag's row span is its cells");
    check(!select_row_span(&g_select, &g_term, 1, &from, &to), "a row outside the drag has no span");

    drag(0, 11, 0, 6);
    check(copies("world"), "a drag toward the start copies the same way");

    reset(4, 20, "one\r\ntwo\r\nthree");
    drag(0, 1, 2, 2);
    check(copies("ne\ntwo\nth"), "a drag across rows joins them by newlines");

    drag(0, 0, 1, 0);
    check(copies("one\n"), "a drag to the next row's start takes the line end and drops the blanks before it");
    check(select_row_span(&g_select, &g_term, 0, &from, &to) && from == 0 && to == 4,
          "the text lights with one cell for the line break");
    check(!select_row_span(&g_select, &g_term, 1, &from, &to), "the next row lights nothing");

    drag(0, 0, 0, 20);
    check(copies("one"), "the blanks after a row's text stay out");
    check(select_row_span(&g_select, &g_term, 0, &from, &to) && to == 3, "the blanks after the text stay unlit");

    reset(4, 20, "a   b");
    drag(0, 0, 0, 5);
    check(copies("a   b"), "blanks between words stay in");

    drag(1, 3, 1, 3);
    check(!select_has_text(&g_select), "a click without a drag selects nothing");
}

static void check_words(void) {
    reset(4, 40, "ls /etc/res/fonts -la (x) a   b");
    select_begin(&g_select, &g_term, at(0, 6), SELECT_BY_WORD);
    check(copies("/etc/res/fonts"), "a double click takes a whole path");

    select_begin(&g_select, &g_term, at(0, 19), SELECT_BY_WORD);
    check(copies("-la"), "a double click takes an option with its dash");

    select_begin(&g_select, &g_term, at(0, 22), SELECT_BY_WORD);
    check(copies("("), "a double click on punctuation takes that character");

    select_begin(&g_select, &g_term, at(0, 28), SELECT_BY_WORD);
    check(copies("   "), "a double click on blanks takes the run of blanks");

    select_begin(&g_select, &g_term, at(0, 1), SELECT_BY_WORD);
    select_drag(&g_select, &g_term, at(0, 14));
    check(copies("ls /etc/res/fonts"), "dragging a double click grows by whole words");

    select_drag(&g_select, &g_term, at(0, 0));
    check(copies("ls"), "dragging back keeps the first word");

    reset(4, 40, "warning in hello.c:5 for $HOME");
    select_begin(&g_select, &g_term, at(0, 12), SELECT_BY_WORD);
    check(copies("hello.c"), "a colon ends a word");

    select_begin(&g_select, &g_term, at(0, 27), SELECT_BY_WORD);
    check(copies("HOME"), "a dollar sign ends a word");

    reset(4, 10, "xx /usr/lib/longname");
    select_begin(&g_select, &g_term, at(1, 3), SELECT_BY_WORD);
    check(copies("/usr/lib/longname"), "a word that wrapped selects whole");
}

static void check_lines(void) {
    reset(4, 10, "abcdefghijKLMNO\r\nnext");
    select_begin(&g_select, &g_term, at(1, 2), SELECT_BY_LINE);
    check(copies("abcdefghijKLMNO"), "a triple click takes the whole line it wrapped across, without its end");

    select_drag(&g_select, &g_term, at(2, 0));
    check(copies("abcdefghijKLMNO\nnext"), "dragging a triple click grows by whole lines");

    drag(0, 8, 1, 2);
    check(copies("ijKL"), "a drag across a wrap joins the rows without a newline");
}

static void check_extend(void) {
    reset(4, 20, "hello world");
    drag(0, 0, 0, 5);
    select_extend(&g_select, &g_term, at(0, 11));
    check(copies("hello world"), "Shift past the end moves the end");

    select_extend(&g_select, &g_term, at(0, 2));
    check(copies("llo world"), "Shift inside, near the start, moves the start");

    select_extend(&g_select, &g_term, at(0, 9));
    check(copies("llo wor"), "Shift inside, near the end, moves the end");

    select_begin(&g_select, &g_term, at(0, 7), SELECT_BY_WORD);
    select_extend(&g_select, &g_term, at(0, 1));
    check(copies("hello world"), "Shift after a double click extends by whole words");

    reset(4, 20, "one\r\ntwo\r\nthree\r\nfour");
    select_begin(&g_select, &g_term, at(0, 1), SELECT_BY_LINE);
    select_extend(&g_select, &g_term, at(3, 1));
    check(copies("one\ntwo\nthree\nfour"), "Shift below a triple click extends down by whole lines");

    select_extend(&g_select, &g_term, at(2, 1));
    check(copies("one\ntwo\nthree"), "Shift inside, near the end, pulls the end back a line");
}

static void check_follow(void) {
    reset(3, 20, "a1\r\nb2\r\nc3");
    select_begin(&g_select, &g_term, at(1, 0), SELECT_BY_LINE);
    feed("\r\nd4");

    int from = -1;
    int to = -1;
    check(select_row_span(&g_select, &g_term, 0, &from, &to) && from == 0, "a selection moves up with its text");
    check(copies("b2"), "a scrolled selection copies the same text");

    feed("\r\n");
    check(!select_has_text(&g_select), "a selection scrolled off the top ends");

    reset(3, 20, "a1\r\nb2\r\nc3");
    drag(0, 0, 1, 2);
    feed("\r\nd4");
    check(copies("b2"), "a selection partly scrolled off keeps what is left");

    reset(3, 20, "hello world");
    drag(0, 0, 0, 5);
    feed("\rhello");
    check(copies("hello"), "rewriting the same text keeps the selection");

    feed("\x1b[1;7HWORLD");
    check(copies("hello"), "a change beside the selection keeps it");

    feed("\r\x1b[2Kbye");
    check(!select_has_text(&g_select), "a change under the selection ends it");

    reset(3, 20, "a1\r\nb2\r\nc3");
    select_begin(&g_select, &g_term, at(0, 1), SELECT_BY_CELL);
    feed("\r\nd4");
    check(g_select.active, "a press not yet dragged survives its row scrolling away");
    select_drag(&g_select, &g_term, at(1, 2));
    check(copies("b2\nc3"), "the drag then starts from the top");
}

int main(void) {
    check_cells();
    check_words();
    check_lines();
    check_extend();
    check_follow();

    if (g_failures != 0) {
        printf("select: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("select: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
