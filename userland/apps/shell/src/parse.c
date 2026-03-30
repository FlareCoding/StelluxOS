#include "parse.h"
#include <stddef.h>

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
