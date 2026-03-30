#ifndef STLX_PARSE_H
#define STLX_PARSE_H

#define MAX_ARGS 32
#define MAX_PIPE_STAGES 8

/* Tokenize line in-place (modifies line). Fills argv with pointers into
   line. Returns argc (number of tokens), 0 if empty. */
int parse_line(char* line, const char* argv[MAX_ARGS + 1]);

/* Split a line into pipeline stages on unquoted '|'. Writes NUL-terminated
   stage pointers into stages[]. Returns the number of stages (1 = no pipe). */
int parse_pipeline(char* line, char* stages[MAX_PIPE_STAGES]);

#endif /* STLX_PARSE_H */
