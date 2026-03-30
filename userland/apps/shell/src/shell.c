#include <stlx/proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "line_edit.h"
#include "parse.h"
#include "builtins.h"

#define STLX_TCSETS_RAW    0x5401
#define STLX_TCSETS_COOKED 0x5402

static void shell_err(const char* s) {
    write(1, s, strlen(s));
}

static const char* resolve_cmd(const char* name, char* path_buf, int buf_size) {
    if (strchr(name, '/')) return name;
    int n = snprintf(path_buf, buf_size, "/bin/%s", name);
    return (n > 0 && n < buf_size) ? path_buf : name;
}

/*
 * Open redirect files described by redir.
 * Sets *in_fd / *out_fd to the opened fd, or -1 if no redirect.
 * Returns 0 on success, -1 on error (prints a message).
 */
static int open_redirect_fds(const redirect_info* redir, int* in_fd, int* out_fd) {
    *in_fd  = -1;
    *out_fd = -1;

    if (redir->stdout_mode == REDIR_OUT) {
        *out_fd = open(redir->stdout_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (*out_fd < 0) {
            shell_err("shell: cannot open '");
            shell_err(redir->stdout_file);
            shell_err("' for writing\r\n");
            return -1;
        }
    } else if (redir->stdout_mode == REDIR_APPEND) {
        *out_fd = open(redir->stdout_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (*out_fd < 0) {
            shell_err("shell: cannot open '");
            shell_err(redir->stdout_file);
            shell_err("' for appending\r\n");
            return -1;
        }
    }

    if (redir->stdin_mode == REDIR_IN) {
        *in_fd = open(redir->stdin_file, O_RDONLY);
        if (*in_fd < 0) {
            shell_err("shell: cannot open '");
            shell_err(redir->stdin_file);
            shell_err("' for reading\r\n");
            if (*out_fd >= 0) { close(*out_fd); *out_fd = -1; }
            return -1;
        }
    }

    return 0;
}

static void close_redirect_fds(int in_fd, int out_fd) {
    if (in_fd >= 0)  close(in_fd);
    if (out_fd >= 0) close(out_fd);
}

static int run_single(const char* argv[], char* path_buf,
                       const redirect_info* redir) {
    int redir_in = -1, redir_out = -1;
    if (open_redirect_fds(redir, &redir_in, &redir_out) < 0)
        return 1;

    const char* cmd = resolve_cmd(argv[0], path_buf, 256);

    int handle = proc_create(cmd, argv + 1);
    if (handle < 0) {
        close_redirect_fds(redir_in, redir_out);
        shell_err(argv[0]);
        shell_err(": command not found\r\n");
        return 127;
    }

    if (redir_in >= 0)
        proc_set_handle(handle, STDIN_FILENO, redir_in);
    if (redir_out >= 0)
        proc_set_handle(handle, STDOUT_FILENO, redir_out);

    if (proc_start(handle) < 0) {
        close(handle);
        close_redirect_fds(redir_in, redir_out);
        shell_err("shell: failed to start process\r\n");
        return 126;
    }

    close_redirect_fds(redir_in, redir_out);

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
        /* Parse redirections first — modifies stage string in-place */
        redirect_info redir;
        if (parse_redirects(stages[i], &redir) < 0) {
            shell_err("shell: syntax error in redirection\r\n");
            if (prev_read_fd >= 0) close(prev_read_fd);
            for (int j = 0; j < i; j++) proc_detach(handles[j]);
            return 1;
        }

        const char* argv[MAX_ARGS + 1];
        int argc = parse_line(stages[i], argv);
        if (argc <= 0) {
            shell_err("shell: empty pipeline stage\r\n");
            if (prev_read_fd >= 0) close(prev_read_fd);
            for (int j = 0; j < i; j++) proc_detach(handles[j]);
            return 1;
        }

        const char* cmd = resolve_cmd(argv[0], path_buf, 256);

        /* Create inter-stage pipe (except for last stage) */
        int pipe_fds[2] = {-1, -1};
        if (i < nstages - 1) {
            if (pipe(pipe_fds) < 0) {
                shell_err("shell: pipe failed\r\n");
                if (prev_read_fd >= 0) close(prev_read_fd);
                for (int j = 0; j < i; j++) proc_detach(handles[j]);
                return 1;
            }
        }

        /* Open redirect files */
        int redir_in = -1, redir_out = -1;
        if (open_redirect_fds(&redir, &redir_in, &redir_out) < 0) {
            if (prev_read_fd >= 0) close(prev_read_fd);
            if (pipe_fds[0] >= 0) close(pipe_fds[0]);
            if (pipe_fds[1] >= 0) close(pipe_fds[1]);
            for (int j = 0; j < i; j++) proc_detach(handles[j]);
            return 1;
        }

        int handle = proc_create(cmd, argv + 1);
        if (handle < 0) {
            shell_err(argv[0]);
            shell_err(": command not found\r\n");
            close_redirect_fds(redir_in, redir_out);
            if (prev_read_fd >= 0) close(prev_read_fd);
            if (pipe_fds[0] >= 0) close(pipe_fds[0]);
            if (pipe_fds[1] >= 0) close(pipe_fds[1]);
            for (int j = 0; j < i; j++) proc_detach(handles[j]);
            return 127;
        }

        /* Wire stdin: redirect overrides pipe */
        if (redir_in >= 0) {
            proc_set_handle(handle, STDIN_FILENO, redir_in);
            /* If there was also a pipe input, we still consumed it above;
               prev_read_fd will be closed below. */
        } else if (prev_read_fd >= 0) {
            proc_set_handle(handle, STDIN_FILENO, prev_read_fd);
        }

        /* Wire stdout: redirect overrides pipe */
        if (redir_out >= 0) {
            proc_set_handle(handle, STDOUT_FILENO, redir_out);
        } else if (pipe_fds[1] >= 0) {
            proc_set_handle(handle, STDOUT_FILENO, pipe_fds[1]);
        }

        if (proc_start(handle) < 0) {
            shell_err("shell: failed to start process\r\n");
            close(handle);
            close_redirect_fds(redir_in, redir_out);
            if (prev_read_fd >= 0) close(prev_read_fd);
            if (pipe_fds[0] >= 0) close(pipe_fds[0]);
            if (pipe_fds[1] >= 0) close(pipe_fds[1]);
            for (int j = 0; j < i; j++) proc_detach(handles[j]);
            return 126;
        }
        handles[i] = handle;

        /* Close fds the parent no longer needs */
        close_redirect_fds(redir_in, redir_out);
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
            /* Parse redirections before tokenizing */
            redirect_info redir;
            if (parse_redirects(stages[0], &redir) < 0) {
                shell_err("shell: syntax error in redirection\r\n");
                continue;
            }

            const char* argv[MAX_ARGS + 1];
            int argc = parse_line(stages[0], argv);
            if (argc <= 0) continue;

            /*
             * Try builtins first. We open redirect fds upfront so
             * builtin output can go to the redirect target. If it
             * turns out not to be a builtin, we close the fds and
             * let run_single() handle opening them for the child
             * process (run_single needs its own fds to pass via
             * proc_set_handle).
             */
            int redir_in = -1, redir_out = -1;
            if (open_redirect_fds(&redir, &redir_in, &redir_out) < 0) {
                last_status = 1;
                continue;
            }
            int builtin_out = (redir_out >= 0) ? redir_out : STDOUT_FILENO;

            int builtin_rc = try_builtin(argc, argv, editor,
                                         last_status, &shell_exit_code, builtin_out);
            close_redirect_fds(redir_in, redir_out);

            if (builtin_rc < 0) break;            /* exit */
            if (builtin_rc > 0) {
                last_status = 0;
                continue;
            }

            /* Not a builtin — run as external command */
            last_status = run_single(argv, path_buf, &redir);
        } else {
            last_status = run_pipeline(stages, nstages, path_buf);
        }
    }

    free(path_buf);
    line_edit_destroy(editor);
    ioctl(0, STLX_TCSETS_COOKED, 0);
    return shell_exit_code;
}
