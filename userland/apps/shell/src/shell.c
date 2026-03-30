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

static const char* resolve_cmd(const char* name, char* path_buf, int buf_size) {
    if (strchr(name, '/')) return name;
    int n = snprintf(path_buf, buf_size, "/bin/%s", name);
    return (n > 0 && n < buf_size) ? path_buf : name;
}

static int run_single(const char* argv[], char* path_buf) {
    const char* cmd = resolve_cmd(argv[0], path_buf, 256);

    int handle = proc_create(cmd, argv + 1);
    if (handle < 0) {
        write(1, argv[0], strlen(argv[0]));
        write(1, ": command not found\r\n", 21);
        return 127;
    }

    if (proc_start(handle) < 0) {
        close(handle);
        write(1, "shell: failed to start process\r\n", 32);
        return 126;
    }

    ioctl(0, STLX_TCSETS_COOKED, 0);
    int status = 0;
    proc_wait(handle, &status);
    ioctl(0, STLX_TCSETS_RAW, 0);

    if (STLX_WIFEXITED(status)) return STLX_WEXITSTATUS(status);
    if (STLX_WIFSIGNALED(status)) return 128 + STLX_WTERMSIG(status);
    return status;
}

static int run_pipeline(char* stages[], int nstages, char* path_buf) {
    int handles[MAX_PIPE_STAGES];
    int prev_read_fd = -1;

    for (int i = 0; i < nstages; i++) {
        const char* argv[MAX_ARGS + 1];
        int argc = parse_line(stages[i], argv);
        if (argc <= 0) {
            write(1, "shell: empty pipeline stage\r\n", 29);
            if (prev_read_fd >= 0) close(prev_read_fd);
            for (int j = 0; j < i; j++) proc_detach(handles[j]);
            return 1;
        }

        const char* cmd = resolve_cmd(argv[0], path_buf, 256);

        int pipe_fds[2] = {-1, -1};
        if (i < nstages - 1) {
            if (pipe(pipe_fds) < 0) {
                write(1, "shell: pipe failed\r\n", 20);
                if (prev_read_fd >= 0) close(prev_read_fd);
                for (int j = 0; j < i; j++) proc_detach(handles[j]);
                return 1;
            }
        }

        int handle = proc_create(cmd, argv + 1);
        if (handle < 0) {
            write(1, argv[0], strlen(argv[0]));
            write(1, ": command not found\r\n", 21);
            if (prev_read_fd >= 0) close(prev_read_fd);
            if (pipe_fds[0] >= 0) close(pipe_fds[0]);
            if (pipe_fds[1] >= 0) close(pipe_fds[1]);
            for (int j = 0; j < i; j++) proc_detach(handles[j]);
            return 127;
        }

        if (prev_read_fd >= 0) {
            proc_set_handle(handle, STDIN_FILENO, prev_read_fd);
        }
        if (pipe_fds[1] >= 0) {
            proc_set_handle(handle, STDOUT_FILENO, pipe_fds[1]);
        }

        if (proc_start(handle) < 0) {
            write(1, "shell: failed to start process\r\n", 32);
            close(handle);
            if (prev_read_fd >= 0) close(prev_read_fd);
            if (pipe_fds[0] >= 0) close(pipe_fds[0]);
            if (pipe_fds[1] >= 0) close(pipe_fds[1]);
            for (int j = 0; j < i; j++) proc_detach(handles[j]);
            return 126;
        }
        handles[i] = handle;

        if (prev_read_fd >= 0) close(prev_read_fd);
        if (pipe_fds[1] >= 0) close(pipe_fds[1]);

        prev_read_fd = pipe_fds[0];
    }

    for (int i = 0; i < nstages - 1; i++) {
        proc_detach(handles[i]);
    }

    ioctl(0, STLX_TCSETS_COOKED, 0);
    int status = 0;
    proc_wait(handles[nstages - 1], &status);
    ioctl(0, STLX_TCSETS_RAW, 0);

    if (STLX_WIFEXITED(status)) return STLX_WEXITSTATUS(status);
    if (STLX_WIFSIGNALED(status)) return 128 + STLX_WTERMSIG(status);
    return status;
}

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
        char cwd[256];
        char prompt[300];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(prompt, sizeof(prompt), "%s $ ", cwd);
        } else {
            snprintf(prompt, sizeof(prompt), "$ ");
        }
        char* line = line_edit_read(editor, prompt);
        if (!line) break;
        if (line[0] == '\0') continue;

        char* stages[MAX_PIPE_STAGES];
        int nstages = parse_pipeline(line, stages);

        if (nstages == 1) {
            const char* argv[MAX_ARGS + 1];
            int argc = parse_line(stages[0], argv);
            if (argc <= 0) continue;

            int builtin_rc = try_builtin(argc, argv, editor,
                                         last_status, &shell_exit_code);
            if (builtin_rc < 0) break;
            if (builtin_rc > 0) { last_status = 0; continue; }

            last_status = run_single(argv, path_buf);
        } else {
            last_status = run_pipeline(stages, nstages, path_buf);
        }
    }

    free(path_buf);
    line_edit_destroy(editor);
    ioctl(0, STLX_TCSETS_COOKED, 0);
    return shell_exit_code;
}
