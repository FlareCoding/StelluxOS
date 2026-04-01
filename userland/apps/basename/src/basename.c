#include <stdio.h>
#include <string.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: basename path [suffix]\n");
        return 1;
    }

    char* path = argv[1];
    size_t len = strlen(path);

    /* Strip trailing slashes */
    while (len > 1 && path[len - 1] == '/')
        len--;

    /* Find the last slash before the end */
    const char* base = path;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/' && i + 1 < len)
            base = path + i + 1;
    }
    size_t base_len = len - (size_t)(base - path);

    /* Strip suffix if provided and it doesn't consume the entire base */
    if (argc >= 3) {
        size_t suf_len = strlen(argv[2]);
        if (suf_len > 0 && suf_len < base_len &&
            memcmp(base + base_len - suf_len, argv[2], suf_len) == 0) {
            base_len -= suf_len;
        }
    }

    fwrite(base, 1, base_len, stdout);
    putchar('\n');
    return 0;
}
