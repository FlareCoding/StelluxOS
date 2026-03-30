#ifndef STLX_PARSE_H
#define STLX_PARSE_H

#define MAX_ARGS 32
#define MAX_PIPE_STAGES 8
#define REDIR_PATH_MAX 256

/* Redirect modes */
#define REDIR_NONE   0
#define REDIR_IN     1   /* <  file  */
#define REDIR_OUT    2   /* >  file  */
#define REDIR_APPEND 3   /* >> file  */

typedef struct {
    int  stdin_mode;                  /* REDIR_NONE or REDIR_IN */
    int  stdout_mode;                 /* REDIR_NONE, REDIR_OUT, or REDIR_APPEND */
    char stdin_file[REDIR_PATH_MAX];  /* filename for < */
    char stdout_file[REDIR_PATH_MAX]; /* filename for > or >> */
} redirect_info;

/* Tokenize line in-place (modifies line). Fills argv with pointers into
   line. Returns argc (number of tokens), 0 if empty. */
int parse_line(char* line, const char* argv[MAX_ARGS + 1]);

/* Split a line into pipeline stages on unquoted '|'. Writes NUL-terminated
   stage pointers into stages[]. Returns the number of stages (1 = no pipe). */
int parse_pipeline(char* line, char* stages[MAX_PIPE_STAGES]);

/* Extract I/O redirections from a command string. Modifies `line` in-place
   to blank out redirect tokens so parse_line() won't see them.
   Returns 0 on success, -1 on syntax error (e.g. missing filename). */
int parse_redirects(char* line, redirect_info* redir);

#endif /* STLX_PARSE_H */
