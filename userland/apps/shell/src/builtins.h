#ifndef STLX_BUILTINS_H
#define STLX_BUILTINS_H

#include "line_edit.h"

/* Try to execute a builtin command. Returns 1 if handled, 0 if not a builtin.
   Returns -1 if the shell should exit. */
int try_builtin(int argc, const char* argv[], line_edit_state* editor);

#endif /* STLX_BUILTINS_H */
