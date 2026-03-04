#ifndef STLX_LINE_EDIT_H
#define STLX_LINE_EDIT_H

#define LINE_MAX    1024
#define HISTORY_MAX 64

typedef struct {
    char line_buf[LINE_MAX];
    int  line_len;
    int  cursor_pos;
    char history[HISTORY_MAX][LINE_MAX];
    int  history_count;
    int  history_index;
} line_edit_state;

line_edit_state* line_edit_create(void);
void line_edit_destroy(line_edit_state* s);

/* Read a line with editing support. Returns pointer to internal buffer
   on success (valid until next call), or NULL on EOF (Ctrl-D). */
char* line_edit_read(line_edit_state* s, const char* prompt);

#endif /* STLX_LINE_EDIT_H */
