#include "builtins.h"
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>

/* Error output — always goes to the terminal (fd 1) */
static void shell_write(const char* s) {
    write(1, s, strlen(s));
}

/* Data output — goes to the specified fd (may be a redirect target) */
static void data_write(int fd, const char* s) {
    write(fd, s, strlen(s));
}

static int builtin_cd(int argc, const char* argv[]) {
    const char* path = (argc >= 2) ? argv[1] : "/";
    if (chdir(path) < 0) {
        shell_write("cd: ");
        shell_write(path);
        shell_write(": no such directory\n");
    }
    return 1;
}

static int builtin_pwd(int out_fd) {
    char buf[512];
    if (getcwd(buf, sizeof(buf))) {
        data_write(out_fd, buf);
        write(out_fd, "\r\n", 2);
    } else {
        shell_write("pwd: error\n");
    }
    return 1;
}

static int builtin_history(line_edit_state* editor, int out_fd) {
    int total = editor->history_count < HISTORY_MAX ? editor->history_count : HISTORY_MAX;
    int base = editor->history_count > HISTORY_MAX ? editor->history_count - HISTORY_MAX : 0;

    for (int i = 0; i < total; i++) {
        int slot = (base + i) % HISTORY_MAX;

        char num[12];
        int n = base + i + 1;
        int pos = 0;
        if (n >= 100) num[pos++] = '0' + (n / 100) % 10;
        if (n >= 10) num[pos++] = '0' + (n / 10) % 10;
        num[pos++] = '0' + n % 10;
        num[pos] = '\0';

        write(out_fd, "  ", 2);
        data_write(out_fd, num);
        write(out_fd, "  ", 2);
        data_write(out_fd, editor->history[slot]);
        write(out_fd, "\r\n", 2);
    }
    return 1;
}

static void write_int_fd(int fd, int val) {
    char buf[12];
    int pos = 0;
    if (val < 0) { write(fd, "-", 1); val = -val; }
    do { buf[pos++] = '0' + (val % 10); val /= 10; } while (val > 0);
    while (pos > 0) write(fd, &buf[--pos], 1);
}

int is_builtin(const char* name) {
    return strcmp(name, "cd") == 0 ||
           strcmp(name, "pwd") == 0 ||
           strcmp(name, "history") == 0 ||
           strcmp(name, "echo") == 0 ||
           strcmp(name, "exit") == 0 ||
           strcmp(name, "$?") == 0 ||
           strcmp(name, "$$") == 0;
}

int try_builtin(int argc, const char* argv[], line_edit_state* editor,
                int last_status, int* out_exit_code, int out_fd) {
    if (argc <= 0) return 0;

    if (strcmp(argv[0], "cd") == 0) return builtin_cd(argc, argv);
    if (strcmp(argv[0], "pwd") == 0) return builtin_pwd(out_fd);
    if (strcmp(argv[0], "history") == 0) return builtin_history(editor, out_fd);

    if (strcmp(argv[0], "$?") == 0) {
        write_int_fd(out_fd, last_status);
        write(out_fd, "\r\n", 2);
        return 1;
    }

    if (strcmp(argv[0], "$$") == 0) {
        write_int_fd(out_fd, (int)getpid());
        write(out_fd, "\r\n", 2);
        return 1;
    }

    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) write(out_fd, " ", 1);
            if (strcmp(argv[i], "$?") == 0) {
                write_int_fd(out_fd, last_status);
            } else if (strcmp(argv[i], "$$") == 0) {
                write_int_fd(out_fd, (int)getpid());
            } else {
                write(out_fd, argv[i], strlen(argv[i]));
            }
        }
        write(out_fd, "\r\n", 2);
        return 1;
    }

    if (strcmp(argv[0], "exit") == 0) {
        *out_exit_code = (argc >= 2) ? atoi(argv[1]) : 0;
        return -1;
    }

    return 0;
}
