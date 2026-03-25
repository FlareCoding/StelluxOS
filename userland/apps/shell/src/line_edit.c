#include "line_edit.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_str(const char* s) {
    write(1, s, strlen(s));
}

static void write_chars(const char* buf, int len) {
    if (len > 0) write(1, buf, (size_t)len);
}

static void redraw(line_edit_state* s, const char* prompt) {
    int prompt_len = (int)strlen(prompt);

    write(1, "\r", 1);
    write(1, "\x1b[2K", 4);
    write_chars(prompt, prompt_len);
    write_chars(s->line_buf, s->line_len);

    int back = s->line_len - s->cursor_pos;
    if (back > 0) {
        char seq[16];
        int n = 0;
        seq[n++] = '\x1b';
        seq[n++] = '[';
        char digits[6];
        int dpos = 0;
        int tmp = back;
        do { digits[dpos++] = '0' + (tmp % 10); tmp /= 10; } while (tmp > 0);
        while (dpos > 0) seq[n++] = digits[--dpos];
        seq[n++] = 'D';
        write_chars(seq, n);
    }
}

static void history_add(line_edit_state* s, const char* line) {
    if (line[0] == '\0') return;

    if (s->history_count > 0) {
        int prev = (s->history_count - 1) % HISTORY_MAX;
        if (strcmp(s->history[prev], line) == 0) return;
    }

    int idx = s->history_count % HISTORY_MAX;
    strncpy(s->history[idx], line, LINE_MAX - 1);
    s->history[idx][LINE_MAX - 1] = '\0';
    s->history_count++;
}

static void set_line(line_edit_state* s, const char* text) {
    int len = (int)strlen(text);
    if (len >= LINE_MAX) len = LINE_MAX - 1;
    memcpy(s->line_buf, text, (size_t)len);
    s->line_buf[len] = '\0';
    s->line_len = len;
    s->cursor_pos = len;
}

static int read_byte(void) {
    unsigned char c;
    ssize_t n = read(0, &c, 1);
    if (n <= 0) return -1;
    return c;
}

static int try_read_byte(void) {
    unsigned char c;
    ssize_t n = read(0, &c, 1);
    if (n <= 0) return -1;
    return c;
}

static void handle_escape(line_edit_state* s, const char* prompt) {
    int b = try_read_byte();
    if (b < 0) return;
    if (b != '[') return;

    int code = try_read_byte();
    if (code < 0) return;

    int total = s->history_count < HISTORY_MAX ? s->history_count : HISTORY_MAX;

    switch (code) {
    case 'A': // up
        if (total == 0) break;
        if (s->history_index > 0) {
            s->history_index--;
        }
        {
            int idx = s->history_index;
            int base = s->history_count > HISTORY_MAX ? s->history_count - HISTORY_MAX : 0;
            int slot = (base + idx) % HISTORY_MAX;
            set_line(s, s->history[slot]);
        }
        redraw(s, prompt);
        break;

    case 'B': // down
        if (total == 0) break;
        if (s->history_index >= total) break;
        s->history_index++;
        if (s->history_index >= total) {
            s->line_buf[0] = '\0';
            s->line_len = 0;
            s->cursor_pos = 0;
        } else {
            int idx = s->history_index;
            int base = s->history_count > HISTORY_MAX ? s->history_count - HISTORY_MAX : 0;
            int slot = (base + idx) % HISTORY_MAX;
            set_line(s, s->history[slot]);
        }
        redraw(s, prompt);
        break;

    case 'C': // right
        if (s->cursor_pos < s->line_len) {
            s->cursor_pos++;
            write(1, "\x1b[C", 3);
        }
        break;

    case 'D': // left
        if (s->cursor_pos > 0) {
            s->cursor_pos--;
            write(1, "\x1b[D", 3);
        }
        break;

    case 'H': // Home
        s->cursor_pos = 0;
        redraw(s, prompt);
        break;

    case 'F': // End
        s->cursor_pos = s->line_len;
        redraw(s, prompt);
        break;

    case '3': { // Delete key sends ESC[3~
        int tilde = try_read_byte();
        if (tilde != '~') break;
        if (s->cursor_pos < s->line_len) {
            memmove(s->line_buf + s->cursor_pos,
                    s->line_buf + s->cursor_pos + 1,
                    (size_t)(s->line_len - s->cursor_pos - 1));
            s->line_len--;
            s->line_buf[s->line_len] = '\0';
            redraw(s, prompt);
        }
        break;
    }
    }
}

line_edit_state* line_edit_create(void) {
    line_edit_state* s = calloc(1, sizeof(line_edit_state));
    return s;
}

void line_edit_destroy(line_edit_state* s) {
    free(s);
}

char* line_edit_read(line_edit_state* s, const char* prompt) {
    s->line_buf[0] = '\0';
    s->line_len = 0;
    s->cursor_pos = 0;
    s->history_index = s->history_count < HISTORY_MAX ? s->history_count : HISTORY_MAX;

    write_str(prompt);

    for (;;) {
        int c = read_byte();
        if (c < 0) return NULL;

        if (c == 0x04) { // Ctrl-D
            if (s->line_len == 0) return NULL;
            continue;
        }

        if (c == '\r' || c == '\n') {
            write(1, "\n", 1);
            s->line_buf[s->line_len] = '\0';
            history_add(s, s->line_buf);
            return s->line_buf;
        }

        if (c == 0x1b) {
            handle_escape(s, prompt);
            continue;
        }

        if (c == 0x7f || c == 0x08) { // Backspace
            if (s->cursor_pos > 0) {
                memmove(s->line_buf + s->cursor_pos - 1,
                        s->line_buf + s->cursor_pos,
                        (size_t)(s->line_len - s->cursor_pos));
                s->cursor_pos--;
                s->line_len--;
                s->line_buf[s->line_len] = '\0';
                redraw(s, prompt);
            }
            continue;
        }

        if (c == 0x01) { // Ctrl-A: Home
            s->cursor_pos = 0;
            redraw(s, prompt);
            continue;
        }

        if (c == 0x05) { // Ctrl-E: End
            s->cursor_pos = s->line_len;
            redraw(s, prompt);
            continue;
        }

        if (c == 0x0b) { // Ctrl-K: kill to end of line
            s->line_len = s->cursor_pos;
            s->line_buf[s->line_len] = '\0';
            redraw(s, prompt);
            continue;
        }

        if (c == 0x15) { // Ctrl-U: kill whole line
            s->line_len = 0;
            s->cursor_pos = 0;
            s->line_buf[0] = '\0';
            redraw(s, prompt);
            continue;
        }

        if (c >= 0x20 && c <= 0x7e) {
            if (s->line_len >= LINE_MAX - 1) continue;
            memmove(s->line_buf + s->cursor_pos + 1,
                    s->line_buf + s->cursor_pos,
                    (size_t)(s->line_len - s->cursor_pos));
            s->line_buf[s->cursor_pos] = (char)c;
            s->cursor_pos++;
            s->line_len++;
            s->line_buf[s->line_len] = '\0';
            redraw(s, prompt);
        }
    }
}
