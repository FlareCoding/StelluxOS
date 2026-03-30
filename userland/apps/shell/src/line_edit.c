#include "line_edit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

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

/* ---- Tab completion ---- */

#define COMPLETE_MAX 128
#define COMPLETE_NAME_MAX 256

typedef struct {
    char name[COMPLETE_NAME_MAX];
    int  is_dir;
} completion_entry;

/*
 * Determine if the current token is in "command position": the first
 * token of the current pipeline stage. We scan backwards from tok_start
 * for an unquoted '|' or the start of line, then check if there's only
 * whitespace between that boundary and tok_start.
 */
static int is_command_position(const char* buf, int tok_start) {
    int scan = tok_start;
    while (scan > 0) {
        scan--;
        if (buf[scan] == '|') {
            scan++; /* position after the pipe */
            break;
        }
    }
    /* skip whitespace from boundary to tok_start */
    while (scan < tok_start && (buf[scan] == ' ' || buf[scan] == '\t'))
        scan++;
    return (scan == tok_start);
}

/*
 * Collect completion candidates from a directory.
 * Filters entries whose name starts with `prefix` (prefix_len chars).
 * Returns the number of matches stored in `out`.
 */
static int collect_candidates(const char* dir_path, const char* prefix,
                              int prefix_len, completion_entry* out, int max) {
    DIR* dir = opendir(dir_path);
    if (!dir) return 0;

    int count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        /* skip . and .. */
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0') continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0') continue;
            /* skip other dotfiles unless prefix starts with . */
            if (prefix_len == 0 || prefix[0] != '.') continue;
        }

        if (prefix_len > 0 && strncmp(ent->d_name, prefix, (size_t)prefix_len) != 0)
            continue;

        strncpy(out[count].name, ent->d_name, COMPLETE_NAME_MAX - 1);
        out[count].name[COMPLETE_NAME_MAX - 1] = '\0';

        /* Check if it's a directory via stat */
        char full_path[512];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);
        if (n > 0 && n < (int)sizeof(full_path)) {
            struct stat st;
            out[count].is_dir = (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode));
        } else {
            out[count].is_dir = 0;
        }

        count++;
    }

    closedir(dir);
    return count;
}

/*
 * Sort completion entries alphabetically for display.
 */
static int completion_cmp(const void* a, const void* b) {
    const completion_entry* ea = (const completion_entry*)a;
    const completion_entry* eb = (const completion_entry*)b;
    return strcmp(ea->name, eb->name);
}

/*
 * Compute the longest common prefix among all candidates.
 * Returns the length of the common prefix (at least prefix_len).
 */
static int longest_common_prefix(const completion_entry* entries, int count,
                                  int prefix_len) {
    if (count <= 0) return prefix_len;
    if (count == 1) return (int)strlen(entries[0].name);

    int lcp = (int)strlen(entries[0].name);
    for (int i = 1; i < count; i++) {
        int j = 0;
        while (j < lcp && entries[i].name[j] == entries[0].name[j])
            j++;
        lcp = j;
    }
    return lcp;
}

/*
 * Insert text at the cursor position.
 */
static void insert_text(line_edit_state* s, const char* text, int len) {
    if (s->line_len + len >= LINE_MAX) return;
    memmove(s->line_buf + s->cursor_pos + len,
            s->line_buf + s->cursor_pos,
            (size_t)(s->line_len - s->cursor_pos));
    memcpy(s->line_buf + s->cursor_pos, text, (size_t)len);
    s->cursor_pos += len;
    s->line_len += len;
    s->line_buf[s->line_len] = '\0';
}

/*
 * Display candidates in columns below the current line.
 */
static void show_candidates(const completion_entry* entries, int count,
                             line_edit_state* s, const char* prompt) {
    write(1, "\r\n", 2);

    /* find max display length for column sizing */
    int max_len = 0;
    for (int i = 0; i < count; i++) {
        int len = (int)strlen(entries[i].name) + (entries[i].is_dir ? 1 : 0);
        if (len > max_len) max_len = len;
    }

    int col_width = max_len + 2;
    if (col_width < 4) col_width = 4;
    int term_width = 80;
    int num_cols = term_width / col_width;
    if (num_cols < 1) num_cols = 1;

    for (int i = 0; i < count; i++) {
        int name_len = (int)strlen(entries[i].name);
        int display_len = name_len + (entries[i].is_dir ? 1 : 0);
        int last_in_row = ((i + 1) % num_cols == 0) || (i == count - 1);

        if (entries[i].is_dir) {
            /* blue for directories */
            write(1, "\x1b[1;34m", 7);
            write(1, entries[i].name, (size_t)name_len);
            write(1, "/", 1);
            write(1, "\x1b[0m", 4);
        } else {
            write(1, entries[i].name, (size_t)name_len);
        }

        if (last_in_row) {
            write(1, "\r\n", 2);
        } else {
            int padding = col_width - display_len;
            for (int p = 0; p < padding; p++)
                write(1, " ", 1);
        }
    }

    /* redraw prompt + current line */
    redraw(s, prompt);
}

static void handle_tab(line_edit_state* s, const char* prompt) {
    /* Find the start of the current token */
    int tok_start = s->cursor_pos;
    while (tok_start > 0 &&
           s->line_buf[tok_start - 1] != ' ' &&
           s->line_buf[tok_start - 1] != '\t')
        tok_start--;

    int tok_len = s->cursor_pos - tok_start;

    /* Extract the prefix */
    char prefix[LINE_MAX];
    if (tok_len > 0)
        memcpy(prefix, s->line_buf + tok_start, (size_t)tok_len);
    prefix[tok_len] = '\0';

    /* Determine: command completion or path completion? */
    int cmd_mode = 0;
    if (is_command_position(s->line_buf, tok_start) && strchr(prefix, '/') == NULL) {
        cmd_mode = 1;
    }

    /* Split prefix into directory part and name part */
    char dir_path[512];
    char name_prefix[COMPLETE_NAME_MAX];
    int name_prefix_len;

    if (cmd_mode) {
        /* Command mode: search /bin/ for matching executables */
        strncpy(dir_path, "/bin", sizeof(dir_path) - 1);
        dir_path[sizeof(dir_path) - 1] = '\0';
        int cap = tok_len < COMPLETE_NAME_MAX - 1 ? tok_len : COMPLETE_NAME_MAX - 1;
        memcpy(name_prefix, prefix, (size_t)cap);
        name_prefix[cap] = '\0';
        name_prefix_len = cap;
    } else {
        /* File/path mode: split on last '/' */
        const char* last_slash = NULL;
        for (int i = tok_len - 1; i >= 0; i--) {
            if (prefix[i] == '/') { last_slash = prefix + i; break; }
        }

        if (last_slash) {
            int dir_len = (int)(last_slash - prefix) + 1; /* include the slash */
            if (dir_len >= (int)sizeof(dir_path)) dir_len = (int)sizeof(dir_path) - 1;
            memcpy(dir_path, prefix, (size_t)dir_len);
            dir_path[dir_len] = '\0';

            int remaining = tok_len - dir_len;
            if (remaining >= COMPLETE_NAME_MAX) remaining = COMPLETE_NAME_MAX - 1;
            if (remaining > 0)
                memcpy(name_prefix, prefix + dir_len, (size_t)remaining);
            name_prefix[remaining] = '\0';
            name_prefix_len = remaining;
        } else {
            strncpy(dir_path, ".", sizeof(dir_path) - 1);
            dir_path[sizeof(dir_path) - 1] = '\0';
            int cap = tok_len < COMPLETE_NAME_MAX - 1 ? tok_len : COMPLETE_NAME_MAX - 1;
            memcpy(name_prefix, prefix, (size_t)cap);
            name_prefix[cap] = '\0';
            name_prefix_len = cap;
        }
    }

    /* Collect candidates */
    completion_entry* entries = malloc(COMPLETE_MAX * sizeof(completion_entry));
    if (!entries) return;

    int count = collect_candidates(dir_path, name_prefix, name_prefix_len,
                                    entries, COMPLETE_MAX);

    if (count > 1)
        qsort(entries, (size_t)count, sizeof(completion_entry), completion_cmp);

    if (count == 0) {
        /* no matches */
        free(entries);
        return;
    }

    if (count == 1) {
        /* Single match — complete fully */
        const char* match = entries[0].name;
        int match_len = (int)strlen(match);
        int insert_len = match_len - name_prefix_len;
        if (insert_len < 0) insert_len = 0;
        if (insert_len > 0) {
            insert_text(s, match + name_prefix_len, insert_len);
        }
        /* Append / for dirs, space for files */
        if (entries[0].is_dir) {
            insert_text(s, "/", 1);
        } else {
            insert_text(s, " ", 1);
        }
        s->tab_pressed = 0;
        redraw(s, prompt);
        free(entries);
        return;
    }

    /* Multiple matches */
    int lcp = longest_common_prefix(entries, count, name_prefix_len);
    int insert_len = lcp - name_prefix_len;
    if (insert_len < 0) insert_len = 0;

    if (insert_len > 0) {
        /* Complete the common prefix */
        insert_text(s, entries[0].name + name_prefix_len, insert_len);
        s->tab_pressed = 1; /* next Tab shows candidates */
        redraw(s, prompt);
        free(entries);
        return;
    }

    /* No additional common prefix — show candidates on second Tab */
    if (s->tab_pressed) {
        show_candidates(entries, count, s, prompt);
        s->tab_pressed = 0;
    } else {
        s->tab_pressed = 1;
    }

    free(entries);
}

/* ---- Main API ---- */

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
    s->tab_pressed = 0;

    write_str(prompt);

    for (;;) {
        int c = read_byte();
        if (c < 0) return NULL;

        /* Reset double-Tab state on any non-Tab keypress */
        if (c != '\t') s->tab_pressed = 0;

        if (c == 0x04) { // Ctrl-D
            if (s->line_len == 0) return NULL;
            continue;
        }

        if (c == '\r' || c == '\n') {
            write(1, "\r\n", 2);
            s->line_buf[s->line_len] = '\0';
            history_add(s, s->line_buf);
            return s->line_buf;
        }

        if (c == '\t') {
            handle_tab(s, prompt);
            continue;
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
