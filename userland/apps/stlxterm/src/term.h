#ifndef STLXTERM_TERM_H
#define STLXTERM_TERM_H

#include <stdint.h>

#define TERM_MAX_COLS       200
#define TERM_MAX_ROWS       80
#define TERM_CSI_MAX_PARAMS 16
#define TERM_TITLE_MAX      128

typedef struct {
    uint8_t fg; // 0 = default, 1-8 = ANSI 30-37, 9-16 = bright 90-97
    uint8_t bg; // 0 = default, 1-8 = ANSI 40-47, 9-16 = bright 100-107
} term_attr_t;

typedef struct {
    char cells[TERM_MAX_ROWS][TERM_MAX_COLS];
    term_attr_t attrs[TERM_MAX_ROWS][TERM_MAX_COLS];
    int rows;
    int cols;
    int cursor_row;
    int cursor_col;
    int cursor_visible;
    int dirty;

    uint8_t cur_fg;
    uint8_t cur_bg;
    uint8_t cur_bold;
    uint8_t cur_reverse;

    // Scroll region, stored 0-based inclusive
    int scroll_top;
    int scroll_bottom;

    // Saved cursor state (DECSC/DECRC and CSI s/u)
    int saved_cursor_row;
    int saved_cursor_col;
    uint8_t saved_fg;
    uint8_t saved_bg;
    uint8_t saved_bold;
    uint8_t saved_reverse;

    // Alternate screen buffer (DECSET 1049/47)
    char alt_cells[TERM_MAX_ROWS][TERM_MAX_COLS];
    term_attr_t alt_attrs[TERM_MAX_ROWS][TERM_MAX_COLS];
    int alt_cursor_row;
    int alt_cursor_col;
    int using_alt_screen;

    // DECCKM application cursor keys, consulted by the key encoder
    int app_cursor_keys;

    // Parser state
    enum { TERM_ST_NORMAL, TERM_ST_ESC, TERM_ST_CSI, TERM_ST_OSC, TERM_ST_OSC_ESC } state;
    int csi_params[TERM_CSI_MAX_PARAMS];
    int csi_param_count;
    int csi_current_param;
    int csi_private;
    int csi_discard;

    // OSC 0/2 window title, captured for the embedder to consume
    char osc_buf[TERM_TITLE_MAX + 8];
    int osc_len;
    char title[TERM_TITLE_MAX];
    int title_changed;
} term_state_t;

void term_init(term_state_t *t, int rows, int cols);
void term_feed(term_state_t *t, const char *data, int len);

// Reflow to a new grid size, clamping cursor state and resetting the
// scroll region. Content beyond the new bounds is dropped.
void term_resize(term_state_t *t, int rows, int cols);

#endif // STLXTERM_TERM_H
