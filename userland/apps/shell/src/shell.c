#include <stlx/proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "line_edit.h"
#include "parse.h"
#include "builtins.h"

#define STLX_TCSETS_RAW    0x7301
#define STLX_TCSETS_COOKED 0x7302

/* The shell's own process group, restored as foreground after each job */
static int g_shell_pgrp;

static void shell_err(const char* s) {
    write(1, s, strlen(s));
}

static void set_foreground(int pgrp) {
    if (pgrp > 0) tcsetpgrp(STDIN_FILENO, pgrp);
}

/* Put a created (not yet started) child in a process group so terminal
 * signals reach it and not the shell. With pgrp 0 the child leads a new
 * group and becomes the foreground. Returns the group id, or -1. */
static int foreground_child(int handle, int pgrp) {
    process_info info;
    if (proc_info(handle, &info) != 0 ||
        setpgid(info.pid, pgrp > 0 ? pgrp : info.pid) != 0) {
        /* A group-less leader stays in the shell's group: clear the
         * foreground so ^C drops instead of hitting the shell too */
        if (pgrp <= 0) tcsetpgrp(STDIN_FILENO, 0);
        return -1;
    }
    if (pgrp <= 0) {
        pgrp = info.pid;
        set_foreground(pgrp);
    }
    return pgrp;
}

static int reap_status(int status) {
    if (STLX_WIFEXITED(status)) return STLX_WEXITSTATUS(status);
    if (STLX_WIFSIGNALED(status)) {
        int sig = STLX_WTERMSIG(status);
        const char* name;
        switch (sig) {
            case 4:  name = "Illegal instruction";       break;
            case 7:  name = "Bus error";                 break;
            case 8:  name = "Floating point exception"; break;
            case 9:  name = "Killed";                    break;
            case 11: name = "Segmentation fault";        break;
            default: name = "Terminated by signal";      break;
        }
        shell_err(name);
        shell_err("\r\n");
        return 128 + sig;
    }
    return status;
}

/*
 * Resolve a bare command name against PATH, falling back to /bin.
 * Names containing '/' are used as-is.
 */
static const char* resolve_cmd(const char* name, char* path_buf, int buf_size) {
    if (strchr(name, '/')) return name;

    const char* path = getenv("PATH");
    if (!path || !*path) path = "/bin";

    while (*path) {
        const char* sep = strchr(path, ':');
        int dir_len = sep ? (int)(sep - path) : (int)strlen(path);

        if (dir_len > 0) {
            int n = snprintf(path_buf, buf_size, "%.*s/%s", dir_len, path, name);
            if (n > 0 && n < buf_size && access(path_buf, X_OK) == 0)
                return path_buf;
        }

        if (!sep) break;
        path = sep + 1;
    }

    /* Keep the historical candidate so failure messaging is unchanged */
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

    foreground_child(handle, 0);

    if (proc_start(handle) < 0) {
        close(handle);
        close_redirect_fds(redir_in, redir_out);
        set_foreground(g_shell_pgrp);
        shell_err("shell: failed to start process\r\n");
        return 126;
    }

    close_redirect_fds(redir_in, redir_out);

    ioctl(0, STLX_TCSETS_COOKED, 0);
    int status = 0;
    proc_wait(handle, &status);
    ioctl(0, STLX_TCSETS_RAW, 0);
    set_foreground(g_shell_pgrp);

    return reap_status(status);
}

static int run_pipeline(char* stages[], int nstages, char* path_buf) {
    int handles[MAX_PIPE_STAGES];
    int prev_read_fd = -1;
    int fg_pgrp = -1;

    for (int i = 0; i < nstages; i++) {
        /* Parse redirections first — modifies stage string in-place */
        redirect_info redir;
        if (parse_redirects(stages[i], &redir) < 0) {
            shell_err("shell: syntax error in redirection\r\n");
            if (prev_read_fd >= 0) close(prev_read_fd);
            for (int j = 0; j < i; j++) proc_detach(handles[j]);
            if (fg_pgrp > 0) set_foreground(g_shell_pgrp);
            return 1;
        }

        const char* argv[MAX_ARGS + 1];
        int argc = parse_line(stages[i], argv);
        if (argc <= 0) {
            shell_err("shell: empty pipeline stage\r\n");
            if (prev_read_fd >= 0) close(prev_read_fd);
            for (int j = 0; j < i; j++) proc_detach(handles[j]);
            if (fg_pgrp > 0) set_foreground(g_shell_pgrp);
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
                if (fg_pgrp > 0) set_foreground(g_shell_pgrp);
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
            if (fg_pgrp > 0) set_foreground(g_shell_pgrp);
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
            if (fg_pgrp > 0) set_foreground(g_shell_pgrp);
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

        /* First stage leads the foreground group, later stages join it.
         * If the leader setup failed, all stages stay in the shell's
         * group with the foreground cleared. */
        if (i == 0) {
            fg_pgrp = foreground_child(handle, 0);
        } else if (fg_pgrp > 0) {
            foreground_child(handle, fg_pgrp);
        }

        if (proc_start(handle) < 0) {
            shell_err("shell: failed to start process\r\n");
            close(handle);
            close_redirect_fds(redir_in, redir_out);
            if (prev_read_fd >= 0) close(prev_read_fd);
            if (pipe_fds[0] >= 0) close(pipe_fds[0]);
            if (pipe_fds[1] >= 0) close(pipe_fds[1]);
            for (int j = 0; j < i; j++) proc_detach(handles[j]);
            set_foreground(g_shell_pgrp);
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
    set_foreground(g_shell_pgrp);

    return reap_status(status);
}

/* Execute one command line (a pipeline, optionally with builtins and
 * redirections). editor may be NULL when there is no interactive editor
 * (e.g. -c mode). Returns the command's exit status. Sets *should_exit for
 * the `exit` builtin, with *shell_exit_code holding the code. */
static int execute_line(char* line, char* path_buf, line_edit_state* editor,
                        int last_status, int* shell_exit_code, int* should_exit) {
    char* stages[MAX_PIPE_STAGES];
    int nstages = parse_pipeline(line, stages);

    if (nstages != 1) {
        return run_pipeline(stages, nstages, path_buf);
    }

    redirect_info redir;
    if (parse_redirects(stages[0], &redir) < 0) {
        shell_err("shell: syntax error in redirection\r\n");
        return 1;
    }

    const char* argv[MAX_ARGS + 1];
    int argc = parse_line(stages[0], argv);
    if (argc <= 0) return last_status;

    if (is_builtin(argv[0])) {
        int redir_in = -1, redir_out = -1;
        if (open_redirect_fds(&redir, &redir_in, &redir_out) < 0) {
            return 1;
        }
        int builtin_out = (redir_out >= 0) ? redir_out : STDOUT_FILENO;
        int builtin_rc = try_builtin(argc, argv, editor,
                                     last_status, shell_exit_code, builtin_out);
        close_redirect_fds(redir_in, redir_out);
        if (builtin_rc < 0) { *should_exit = 1; return *shell_exit_code; }
        if (builtin_rc > 0) return 0;
        /* fall through to external command */
    }

    return run_single(argv, path_buf, &redir);
}

int main(int argc, char** argv) {
    g_shell_pgrp = getpgid(0);

    /* Non-interactive command mode: `shell -c "command line"` (ssh exec). */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        char* cpath = malloc(256);
        if (!cpath) return 1;
        int exit_code = 0, should_exit = 0;
        /* argv strings are modifiable; execute_line tokenizes in place */
        int status = execute_line(argv[2], cpath, NULL, 0, &exit_code, &should_exit);
        free(cpath);
        return should_exit ? exit_code : status;
    }

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

        int should_exit = 0;
        last_status = execute_line(line, path_buf, editor, last_status,
                                   &shell_exit_code, &should_exit);
        if (should_exit) break;
    }

    free(path_buf);
    line_edit_destroy(editor);
    ioctl(0, STLX_TCSETS_COOKED, 0);
    return shell_exit_code;
}
