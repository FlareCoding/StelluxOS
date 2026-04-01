#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int is_all_digits(const char* s) {
    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        s++;
    }
    return 1;
}

static void head_stream(FILE* fp, int n) {
    char line[4096];
    int count = 0;
    while (count < n && fgets(line, sizeof(line), fp)) {
        fputs(line, stdout);
        count++;
    }
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int n = 10;
    int file_start = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            n = atoi(argv[++i]);
            if (n <= 0) n = 10;
        } else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
            n = atoi(argv[i] + 1);
            if (n <= 0) n = 10;
        } else if (file_start < 0 && is_all_digits(argv[i])) {
            n = atoi(argv[i]);
            if (n <= 0) n = 10;
        } else {
            file_start = i;
            break;
        }
    }

    if (file_start < 0) {
        head_stream(stdin, n);
        return 0;
    }

    int rc = 0;
    int nfiles = argc - file_start;
    for (int i = file_start; i < argc; i++) {
        FILE* fp = fopen(argv[i], "r");
        if (!fp) {
            fprintf(stderr, "head: %s: cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        if (nfiles > 1)
            printf("==> %s <==\n", argv[i]);
        head_stream(fp, n);
        fclose(fp);
    }

    return rc;
}
