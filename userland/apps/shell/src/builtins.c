#include "builtins.h"
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

static void shell_write(const char* s) {
    write(1, s, strlen(s));
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

static int builtin_pwd(void) {
    char buf[512];
    if (getcwd(buf, sizeof(buf))) {
        shell_write(buf);
        write(1, "\r\n", 2);
    } else {
        shell_write("pwd: error\n");
    }
    return 1;
}

static int builtin_history(line_edit_state* editor) {
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

        write(1, "  ", 2);
        shell_write(num);
        write(1, "  ", 2);
        shell_write(editor->history[slot]);
        write(1, "\r\n", 2);
    }
    return 1;
}

static void write_int(int val) {
    char buf[12];
    int pos = 0;
    if (val < 0) { write(1, "-", 1); val = -val; }
    do { buf[pos++] = '0' + (val % 10); val /= 10; } while (val > 0);
    while (pos > 0) write(1, &buf[--pos], 1);
}

int try_builtin(int argc, const char* argv[], line_edit_state* editor,
                int last_status, int* out_exit_code) {
    if (argc <= 0) return 0;

    if (strcmp(argv[0], "cd") == 0) return builtin_cd(argc, argv);
    if (strcmp(argv[0], "pwd") == 0) return builtin_pwd();
    if (strcmp(argv[0], "history") == 0) return builtin_history(editor);

    if (strcmp(argv[0], "$?") == 0) {
        write_int(last_status);
        write(1, "\r\n", 2);
        return 1;
    }

    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) write(1, " ", 1);
            if (strcmp(argv[i], "$?") == 0) {
                write_int(last_status);
            } else {
                write(1, argv[i], strlen(argv[i]));
            }
        }
        write(1, "\r\n", 2);
        return 1;
    }

    if (strcmp(argv[0], "exit") == 0) {
        *out_exit_code = (argc >= 2) ? atoi(argv[1]) : 0;
        return -1;
    }

    return 0;
}
