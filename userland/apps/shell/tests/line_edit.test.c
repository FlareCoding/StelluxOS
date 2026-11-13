/* The line editor's erasing keys, typed as a terminal sends their bytes, with the echo thrown away */
#include "line_edit.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_REPORTS 8

static uint64_t g_checks;
static uint64_t g_failures;

static void check(int holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

/* The line the editor returns after reading `keys`, or NULL when the keys end before a line does or cannot be sent */
static const char* edit_line(line_edit_state* editor, const char* keys) {
    int input[2];
    if (pipe(input) != 0) {
        return NULL;
    }

    ssize_t written = write(input[1], keys, strlen(keys));
    close(input[1]);
    if (written != (ssize_t)strlen(keys)) {
        close(input[0]);
        return NULL;
    }

    int saved_stdin = dup(STDIN_FILENO);
    int saved_stdout = dup(STDOUT_FILENO);
    int null_fd = open("/dev/null", O_WRONLY);
    dup2(input[0], STDIN_FILENO);
    dup2(null_fd, STDOUT_FILENO);

    const char* line = line_edit_read(editor, "$ ");

    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(null_fd);
    close(input[0]);

    return line;
}

static int edits_to(line_edit_state* editor, const char* keys, const char* expected) {
    const char* line = edit_line(editor, keys);
    return line && strcmp(line, expected) == 0;
}

static void check_word_erase(line_edit_state* editor) {
    check(edits_to(editor, "echo alpha beta\x17gamma\r", "echo alpha gamma"),
          "Ctrl W erases the word before the cursor, and typing goes in its place");
    check(edits_to(editor, "echo alpha   \x17\r", "echo "), "Ctrl W takes the spaces before the cursor with the word");
    check(edits_to(editor, "cd /usr/share/fonts\x17\r", "cd "), "Ctrl W erases a whole path");
    check(edits_to(editor, "alpha beta\x1b[D\x1b[D\x17\r", "alpha ta"), "Ctrl W keeps the text after the cursor");
    check(edits_to(editor, "alpha\x01\x17\r", "alpha"), "Ctrl W at the start of the line erases nothing");
    check(edits_to(editor, "alpha\b\x7f\r", "alp"), "both backspace bytes erase one character");
}

int main(void) {
    line_edit_state* editor = line_edit_create();
    if (!editor) {
        printf("line_edit: no memory for the editor\n");
        return 1;
    }

    check_word_erase(editor);
    line_edit_destroy(editor);

    if (g_failures != 0) {
        printf("line_edit: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("line_edit: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
