#ifndef STLX_BUILTINS_H
#define STLX_BUILTINS_H

#include "line_edit.h"

/* Check if a command name is a shell builtin (without executing it). */
int is_builtin(const char* name);

/* Try to execute a builtin command. Returns 1 if handled, 0 if not a builtin.
   Returns -1 if the shell should exit (exit code written to *out_exit_code).
   out_fd: file descriptor for data output (echo, pwd, etc.). Error messages
   from builtins like cd always go to fd 1 (the terminal). */
int try_builtin(int argc, const char* argv[], line_edit_state* editor,
                int last_status, int* out_exit_code, int out_fd);

#endif /* STLX_BUILTINS_H */
