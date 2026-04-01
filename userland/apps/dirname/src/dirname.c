#include <stdio.h>
#include <string.h>

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        fprintf(stderr, "usage: dirname path\n");
        return 1;
    }

    char* path = argv[1];
    size_t len = strlen(path);

    /* Strip trailing slashes (but not if the entire string is slashes) */
    while (len > 1 && path[len - 1] == '/')
        len--;

    /* Find the last slash */
    size_t last_slash = 0;
    int found = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') {
            last_slash = i;
            found = 1;
        }
    }

    if (!found) {
        puts(".");
        return 0;
    }

    /* Strip trailing slashes from the directory part */
    size_t dir_len = last_slash;
    while (dir_len > 1 && path[dir_len - 1] == '/')
        dir_len--;

    if (dir_len == 0 && path[0] == '/')
        dir_len = 1;

    fwrite(path, 1, dir_len, stdout);
    putchar('\n');
    return 0;
}
