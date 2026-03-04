#include "parse.h"
#include <stddef.h>

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
