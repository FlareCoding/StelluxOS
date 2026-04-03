#include "parse.h"
#include <stddef.h>
#include <string.h>

/* ---- Redirect parsing ----
 *
 * Scans a command string for unquoted < , > , >> operators.
 * Extracts the target filename and blanks the redirect region with spaces
 * so that parse_line() never sees the redirect tokens.
 *
 * Handles:
 *   cmd > file       (truncate)
 *   cmd >> file      (append)
 *   cmd < file       (input)
 *   cmd >file        (no space before filename)
 *   cmd >"file"      (quoted filename)
 *   combinations:    cmd < in > out
 */

/* Read a filename token starting at *pos. Advances *pos past it.
   Copies into dst (up to dst_size-1 chars). Returns length, or -1. */
static int read_filename(char* line, int* pos, char* dst, int dst_size) {
    /* skip whitespace between operator and filename */
    while (line[*pos] == ' ' || line[*pos] == '\t')
        (*pos)++;

    if (line[*pos] == '\0') return -1; /* missing filename */

    int out = 0;

    if (line[*pos] == '"' || line[*pos] == '\'') {
        char q = line[*pos];
        (*pos)++;  /* skip opening quote */
        while (line[*pos] && line[*pos] != q) {
            if (out < dst_size - 1) dst[out++] = line[*pos];
            (*pos)++;
        }
        if (line[*pos] == q) (*pos)++;  /* skip closing quote */
    } else {
        while (line[*pos] && line[*pos] != ' ' && line[*pos] != '\t' &&
               line[*pos] != '>' && line[*pos] != '<' && line[*pos] != '|') {
            if (out < dst_size - 1) dst[out++] = line[*pos];
            (*pos)++;
        }
    }

    if (out == 0) return -1;
    dst[out] = '\0';
    return out;
}

int parse_redirects(char* line, redirect_info* redir) {
    redir->stdin_mode  = REDIR_NONE;
    redir->stdout_mode = REDIR_NONE;
    redir->stdin_file[0]  = '\0';
    redir->stdout_file[0] = '\0';

    char in_quote = 0;
    int i = 0;

    while (line[i]) {
        /* track quoting */
        if (!in_quote && (line[i] == '"' || line[i] == '\'')) {
            in_quote = line[i];
            i++;
            continue;
        }
        if (in_quote && line[i] == in_quote) {
            in_quote = 0;
            i++;
            continue;
        }
        if (in_quote) {
            i++;
            continue;
        }

        if (line[i] == '>') {
            int start = i;
            i++;
            int mode = REDIR_OUT;
            if (line[i] == '>') {
                mode = REDIR_APPEND;
                i++;
            }

            char filename[REDIR_PATH_MAX];
            int fn_start = i;
            int fn_len = read_filename(line, &i, filename, REDIR_PATH_MAX);
            if (fn_len < 0) return -1;
            (void)fn_start;

            redir->stdout_mode = mode;
            memcpy(redir->stdout_file, filename, (size_t)(fn_len + 1));

            /* blank the redirect region */
            for (int j = start; j < i; j++)
                line[j] = ' ';
            continue;
        }

        if (line[i] == '<') {
            int start = i;
            i++;

            char filename[REDIR_PATH_MAX];
            int fn_start = i;
            int fn_len = read_filename(line, &i, filename, REDIR_PATH_MAX);
            if (fn_len < 0) return -1;
            (void)fn_start;

            redir->stdin_mode = REDIR_IN;
            memcpy(redir->stdin_file, filename, (size_t)(fn_len + 1));

            /* blank the redirect region */
            for (int j = start; j < i; j++)
                line[j] = ' ';
            continue;
        }

        i++;
    }

    return 0;
}

int parse_pipeline(char* line, char* stages[MAX_PIPE_STAGES]) {
    int count = 0;
    char* p = line;
    char in_quote = 0;

    stages[count++] = p;

    while (*p && count < MAX_PIPE_STAGES) {
        if (!in_quote && (*p == '"' || *p == '\'')) {
            in_quote = *p;
        } else if (in_quote && *p == in_quote) {
            in_quote = 0;
        } else if (!in_quote && *p == '|') {
            *p = '\0';
            p++;
            while (*p == ' ' || *p == '\t') p++;
            stages[count++] = p;
            continue;
        }
        p++;
    }

    return count;
}

int parse_line(char* line, const char* argv[MAX_ARGS + 1]) {
    int argc = 0;

    while (*line && argc < MAX_ARGS) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0') break;

        if (*line == '"') {
            line++;
            argv[argc++] = line;
            while (*line && *line != '"') line++;
            if (*line == '"') *line++ = '\0';
        } else if (*line == '\'') {
            line++;
            argv[argc++] = line;
            while (*line && *line != '\'') line++;
            if (*line == '\'') *line++ = '\0';
        } else {
            argv[argc++] = line;
            while (*line && *line != ' ' && *line != '\t') line++;
            if (*line) *line++ = '\0';
        }
    }

    argv[argc] = NULL;
    return argc;
}
