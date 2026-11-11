#include "select.h"
#include <stdlib.h>
#include <string.h>

// Characters that end a word for a double click, so paths and options select whole
#define WORD_BOUNDARIES "'\"`|:;,()[]{}<>$"

enum { CLASS_SPACE, CLASS_WORD, CLASS_BOUNDARY };

static int char_class(char c) {
    if (c == ' ') {
        return CLASS_SPACE;
    }

    if (strchr(WORD_BOUNDARIES, c)) {
        return CLASS_BOUNDARY;
    }

    return CLASS_WORD;
}

static select_pos_t pos_at(const term_state_t *t, int row, int col) {
    select_pos_t pos = { t->scrolled + (uint64_t)row, col };
    return pos;
}

static int pos_before(select_pos_t a, select_pos_t b) {
    return a.line < b.line || (a.line == b.line && a.col < b.col);
}

static int pos_equal(select_pos_t a, select_pos_t b) {
    return a.line == b.line && a.col == b.col;
}

static select_pos_t pos_min(select_pos_t a, select_pos_t b) {
    return pos_before(b, a) ? b : a;
}

static select_pos_t pos_max(select_pos_t a, select_pos_t b) {
    return pos_before(a, b) ? b : a;
}

// Cells from a to b, negative when b comes first
static int64_t pos_distance(const term_state_t *t, select_pos_t a, select_pos_t b) {
    return (int64_t)(b.line - a.line) * (t->cols + 1) + (b.col - a.col);
}

// The run of cells of one class around a cell, following rows that wrap onto the next
static void word_bounds(const term_state_t *t, int row, int cell, select_pos_t *from, select_pos_t *to) {
    int cls = char_class(t->cells[row][cell]);
    if (cls == CLASS_BOUNDARY) {
        *from = pos_at(t, row, cell);
        *to = pos_at(t, row, cell + 1);
        return;
    }

    int r = row;
    int c = cell;
    while (1) {
        if (c > 0 && char_class(t->cells[r][c - 1]) == cls) {
            c--;
        } else if (c == 0 && r > 0 && t->wrapped[r - 1] && char_class(t->cells[r - 1][t->cols - 1]) == cls) {
            r--;
            c = t->cols - 1;
        } else {
            break;
        }
    }

    *from = pos_at(t, r, c);

    r = row;
    c = cell;
    while (1) {
        if (c < t->cols - 1 && char_class(t->cells[r][c + 1]) == cls) {
            c++;
        } else if (c == t->cols - 1 && r < t->rows - 1 && t->wrapped[r] && char_class(t->cells[r + 1][0]) == cls) {
            r++;
            c = 0;
        } else {
            break;
        }
    }

    *to = pos_at(t, r, c + 1);
}

// A whole line, with the rows it wrapped across
static void line_bounds(const term_state_t *t, int row, select_pos_t *from, select_pos_t *to) {
    int first = row;
    while (first > 0 && t->wrapped[first - 1]) {
        first--;
    }

    int last = row;
    while (last < t->rows - 1 && t->wrapped[last]) {
        last++;
    }

    *from = pos_at(t, first, 0);
    *to = pos_at(t, last, t->cols);
}

static void unit_bounds(const term_state_t *t, select_unit_t unit, select_point_t p,
                        select_pos_t *from, select_pos_t *to) {
    if (unit == SELECT_BY_WORD) {
        word_bounds(t, p.row, p.cell, from, to);
    } else if (unit == SELECT_BY_LINE) {
        line_bounds(t, p.row, from, to);
    } else {
        *from = pos_at(t, p.row, p.edge);
        *to = *from;
    }
}

// Remembers the selected rows as they read now, for select_follow to compare against
static void remember_shown(select_t *s, const term_state_t *t) {
    s->shown_line = s->from.line;
    for (uint64_t line = s->from.line; line <= s->to.line; line++) {
        int row = (int)(line - t->scrolled);
        memcpy(s->shown[line - s->from.line], t->cells[row], (size_t)t->cols);
    }
}

void select_begin(select_t *s, const term_state_t *t, select_point_t p, select_unit_t unit) {
    s->active = 1;
    s->unit = unit;
    unit_bounds(t, unit, p, &s->anchor_from, &s->anchor_to);
    s->from = s->anchor_from;
    s->to = s->anchor_to;
    remember_shown(s, t);
}

int select_drag(select_t *s, const term_state_t *t, select_point_t p) {
    if (!s->active) {
        return 0;
    }

    select_pos_t head_from;
    select_pos_t head_to;
    unit_bounds(t, s->unit, p, &head_from, &head_to);

    select_pos_t from = pos_min(s->anchor_from, head_from);
    select_pos_t to = pos_max(s->anchor_to, head_to);
    if (pos_equal(from, s->from) && pos_equal(to, s->to)) {
        return 0;
    }

    s->from = from;
    s->to = to;
    remember_shown(s, t);
    return 1;
}

void select_extend(select_t *s, const term_state_t *t, select_point_t p) {
    if (!s->active) {
        select_begin(s, t, p, SELECT_BY_CELL);
        return;
    }

    select_pos_t head_from;
    select_pos_t head_to;
    unit_bounds(t, s->unit, p, &head_from, &head_to);

    // A click before the selection moves its start, one after it its end, and one inside it the nearer end
    int moves_start = pos_before(head_from, s->from);
    if (!moves_start && !pos_before(s->to, head_to)) {
        moves_start = pos_distance(t, s->from, head_from) <= pos_distance(t, head_to, s->to);
    }

    s->anchor_from = moves_start ? s->to : s->from;
    s->anchor_to = s->anchor_from;
    select_drag(s, t, p);
}

void select_clear(select_t *s) {
    s->active = 0;
}

int select_has_text(const select_t *s) {
    return s->active && pos_before(s->from, s->to);
}

// The column after a row's last character other than a blank
static int text_end(const term_state_t *t, int row) {
    int end = t->cols;
    while (end > 0 && t->cells[row][end - 1] == ' ') {
        end--;
    }

    return end;
}

int select_row_span(const select_t *s, const term_state_t *t, int row, int *from, int *to) {
    uint64_t line = t->scrolled + (uint64_t)row;
    if (!s->active || line < s->from.line || line > s->to.line) {
        return 0;
    }

    *from = line == s->from.line ? s->from.col : 0;
    *to = line == s->to.line ? s->to.col : t->cols;

    // Blanks a copy trims stay unlit, except one cell standing for a selected line break
    if (line == s->to.line || !t->wrapped[row]) {
        int end = text_end(t, row);
        int lit_end = line == s->to.line ? end : (*from > end ? *from : end) + 1;
        if (*to > lit_end) {
            *to = lit_end;
        }
    }

    return *from < *to;
}

char *select_copy(const select_t *s, const term_state_t *t, size_t *len) {
    if (!select_has_text(s)) {
        return NULL;
    }

    char *text = malloc((size_t)t->rows * (size_t)(t->cols + 1) + 1);
    if (!text) {
        return NULL;
    }

    size_t n = 0;
    for (uint64_t line = s->from.line; line <= s->to.line; line++) {
        int row = (int)(line - t->scrolled);
        int from = line == s->from.line ? s->from.col : 0;
        int to = line == s->to.line ? s->to.col : t->cols;
        int joins_next = line < s->to.line && t->wrapped[row];

        if (!joins_next && to > text_end(t, row)) {
            to = text_end(t, row);
        }

        if (to > from) {
            memcpy(text + n, &t->cells[row][from], (size_t)(to - from));
            n += (size_t)(to - from);
        }

        if (line < s->to.line && !joins_next) {
            text[n++] = '\n';
        }
    }

    text[n] = '\0';
    *len = n;
    return text;
}

void select_follow(select_t *s, const term_state_t *t) {
    if (!s->active) {
        return;
    }

    uint64_t top = t->scrolled;
    if (s->to.line >= top && s->to.line - top >= (uint64_t)t->rows) {
        select_clear(s);
        return;
    }

    // Edges above the top move to it, and a selection all of whose text went ends
    int had_text = select_has_text(s);
    select_pos_t first = pos_at(t, 0, 0);
    select_pos_t *edges[] = { &s->from, &s->to, &s->anchor_from, &s->anchor_to };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        if (edges[i]->line < top) {
            *edges[i] = first;
        }
    }

    if (had_text && !select_has_text(s)) {
        select_clear(s);
        return;
    }

    for (uint64_t line = s->from.line; line <= s->to.line; line++) {
        int row = (int)(line - top);
        int from = line == s->from.line ? s->from.col : 0;
        int to = line == s->to.line ? s->to.col : t->cols;
        if (to > from && memcmp(&t->cells[row][from], &s->shown[line - s->shown_line][from], (size_t)(to - from)) != 0) {
            select_clear(s);
            return;
        }
    }
}
