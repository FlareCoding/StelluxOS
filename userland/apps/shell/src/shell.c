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

    char* path_buf = malloc(256);
    if (!path_buf) {
        line_edit_destroy(editor);
        ioctl(0, STLX_TCSETS_COOKED, 0);
        return 1;
    }

    int last_status = 0;
    int shell_exit_code = 0;

    for (;;) {
        char* line = line_edit_read(editor, "$ ");
        if (!line) break;

        if (line[0] == '\0') continue;

        const char* argv[MAX_ARGS + 1];
        int argc = parse_line(line, argv);
        if (argc <= 0) continue;

        int builtin_rc = try_builtin(argc, argv, editor,
                                     last_status, &shell_exit_code);
        if (builtin_rc < 0) break;
        if (builtin_rc > 0) { last_status = 0; continue; }

        const char* cmd = argv[0];
        if (!strchr(cmd, '/')) {
            int n = snprintf(path_buf, 256, "/initrd/bin/%s", cmd);
            if (n > 0 && n < 256) {
                cmd = path_buf;
            }
        }

        int handle = proc_create(cmd, argv + 1);
        if (handle < 0) {
            write(1, argv[0], strlen(argv[0]));
            write(1, ": command not found\n", 20);
            last_status = 127;
            continue;
        }

        int err = proc_start(handle);
        if (err < 0) {
            write(1, "shell: failed to start process\n", 31);
            last_status = 126;
            continue;
        }

        int status = 0;
        proc_wait(handle, &status);
        if (STLX_WIFEXITED(status)) {
            last_status = STLX_WEXITSTATUS(status);
        } else if (STLX_WIFSIGNALED(status)) {
            last_status = 128 + STLX_WTERMSIG(status);
        } else {
            last_status = status;
        }
    }

    free(path_buf);
    line_edit_destroy(editor);
    ioctl(0, STLX_TCSETS_COOKED, 0);
    return shell_exit_code;
}
