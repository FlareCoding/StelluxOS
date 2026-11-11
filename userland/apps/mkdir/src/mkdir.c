#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static int g_parents;

static void strip_trailing_slashes(char* path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
}

// Under -p a directory that already exists counts as made
static int make_dir(const char* path) {
    struct stat st;
    if (g_parents && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 0;
    }

    if (mkdir(path, 0777) < 0) {
        printf("mkdir: cannot create directory '%s': %s\n", path, strerror(errno));
        return 1;
    }

    return 0;
}

// Makes each ancestor in turn by ending the path at its separators,
// skipping the root and the empty names between doubled slashes
static int make_ancestors(char* path) {
    for (char* slash = strchr(path, '/'); slash; slash = strchr(slash + 1, '/')) {
        if (slash == path || slash[-1] == '/') {
            continue;
        }

        *slash = '\0';
        int rc = make_dir(path);
        *slash = '/';
        if (rc) {
            return rc;
        }
    }

    return 0;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (int j = 1; argv[first][j]; j++) {
            switch (argv[first][j]) {
            case 'p':
                g_parents = 1;
                break;
            default:
                printf("mkdir: unknown option '-%c'\n", argv[first][j]);
                return 1;
            }
        }
    }

    if (first == argc) {
        printf("mkdir: missing operand\n");
        return 1;
    }

    int rc = 0;
    for (int i = first; i < argc; i++) {
        strip_trailing_slashes(argv[i]);
        if (g_parents && make_ancestors(argv[i])) {
            rc = 1;
            continue;
        }

        rc |= make_dir(argv[i]);
    }

    return rc;
}
