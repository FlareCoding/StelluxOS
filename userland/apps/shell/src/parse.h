#ifndef STLX_PARSE_H
#define STLX_PARSE_H

#define MAX_ARGS 32

/* Tokenize line in-place (modifies line). Fills argv with pointers into
   line. Returns argc (number of tokens), 0 if empty. */
int parse_line(char* line, const char* argv[MAX_ARGS + 1]);

#endif /* STLX_PARSE_H */
