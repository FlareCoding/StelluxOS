#include <stlx/proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "line_edit.h"
#include "parse.h"
#include "builtins.h"

#define STLX_TCSETS_RAW    0x5401
#define STLX_TCSETS_COOKED 0x5402

int main(void) {
    ioctl(0, STLX_TCSETS_RAW, 0);

    line_edit_state* editor = line_edit_create();
    if (!editor) {
        ioctl(0, STLX_TCSETS_COOKED, 0);
        return 1;
    }

    for (;;) {
        char* line = line_edit_read(editor, "$ ");
        if (!line) break;

        if (line[0] == '\0') continue;

        const char* argv[MAX_ARGS + 1];
        int argc = parse_line(line, argv);
        if (argc <= 0) continue;

        int builtin_rc = try_builtin(argc, argv, editor);
        if (builtin_rc < 0) break;
        if (builtin_rc > 0) continue;

        int handle = proc_create(argv[0], argv + 1);
        if (handle < 0) {
            write(1, argv[0], strlen(argv[0]));
            write(1, ": command not found\r\n", 21);
            continue;
        }

        int err = proc_start(handle);
        if (err < 0) {
            write(1, "shell: failed to start process\r\n", 32);
            continue;
        }

        int exit_code = -1;
        proc_wait(handle, &exit_code);
    }

    line_edit_destroy(editor);
    ioctl(0, STLX_TCSETS_COOKED, 0);
    return 0;
}
