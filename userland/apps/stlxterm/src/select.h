#ifndef STLXTERM_SELECT_H
#define STLXTERM_SELECT_H

#include <stddef.h>
#include <stdint.h>

#include "term.h"

typedef enum {
    SELECT_BY_CELL, // a press and drag
    SELECT_BY_WORD, // a double click
    SELECT_BY_LINE, // a triple click
} select_unit_t;

// A cell edge: the screen row plus the lines scrolled off above it, and the column of the edge
typedef struct {
    uint64_t line;
    int col;
} select_pos_t;

// The pointer on the grid: its row, the cell under it and the cell edge nearest it
typedef struct {
    int row;
    int cell;
    int edge;
} select_point_t;

typedef struct {
    int active;
    select_unit_t unit;
    select_pos_t anchor_from; // the unit the selection grows from
    select_pos_t anchor_to;
    select_pos_t from;        // the selection, from before to
    select_pos_t to;
    char shown[TERM_MAX_ROWS][TERM_MAX_COLS]; // its rows as they read when selected, from shown_line on
    uint64_t shown_line;
} select_t;

// Starts a selection at p by the given unit
void select_begin(select_t *s, const term_state_t *t, select_point_t p, select_unit_t unit);

// Moves the free end to p, growing by the unit the selection began with. Returns 1 when it changed.
int select_drag(select_t *s, const term_state_t *t, select_point_t p);

// Moves the end nearer p to it, as a Shift click does
void select_extend(select_t *s, const term_state_t *t, select_point_t p);

void select_clear(select_t *s);

int select_has_text(const select_t *s);

// The cells of a screen row the selection lights as [*from, *to): its text, and one cell after the text where a
// selected line break follows. Returns 0 when the row has none.
int select_row_span(const select_t *s, const term_state_t *t, int row, int *from, int *to);

// The selected text with each row's trailing blanks dropped and rows joined by newlines, except where a row
// wrapped onto the next. Returns a malloc'd string the caller frees, or NULL when nothing is selected.
char *select_copy(const select_t *s, const term_state_t *t, size_t *len);

// Keeps the selection on its text after output: it moves up with the lines scrolled away, loses the
// ones gone off the top, and ends once the text under it changes
void select_follow(select_t *s, const term_state_t *t);

#endif // STLXTERM_SELECT_H
