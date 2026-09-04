#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

static char g_src[PATH_MAX];
static char g_dst[PATH_MAX];

static void strip_trailing_slashes(char* path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int path_push(char* path, const char* name) {
    size_t len = strlen(path);
    if (len + 1 + strlen(name) >= PATH_MAX) {
        return -1;
    }

    if (len > 0 && path[len - 1] != '/') {
        path[len++] = '/';
    }
    strcpy(path + len, name);

    return 0;
}

// A trailing slash promises the destination is a directory
static int wants_dir(const char* path) {
    size_t len = strlen(path);
    return len > 1 && path[len - 1] == '/';
}

// The kernel rename carries the semantics: replacing files, refusing to
// move a directory beneath itself or onto a populated one
static int move_operand(const char* src, const char* dst, int dst_is_dir) {
    if (strlen(src) >= PATH_MAX || strlen(dst) >= PATH_MAX) {
        printf("mv: path too long: '%s'\n", strlen(src) >= PATH_MAX ? src : dst);
        return 1;
    }

    strcpy(g_src, src);
    strcpy(g_dst, dst);
    strip_trailing_slashes(g_src);
    strip_trailing_slashes(g_dst);

    if (dst_is_dir && path_push(g_dst, base_name(g_src)) < 0) {
        printf("mv: path too long for '%s' under '%s'\n", base_name(g_src), g_dst);
        return 1;
    }

    struct stat st;
    if (!dst_is_dir && wants_dir(dst) && lstat(g_src, &st) == 0 && !S_ISDIR(st.st_mode)) {
        printf("mv: cannot move '%s' to '%s': Not a directory\n", g_src, dst);
        return 1;
    }

    if (rename(g_src, g_dst) < 0) {
        printf("mv: cannot move '%s' to '%s': %s\n", g_src, g_dst, strerror(errno));
        return 1;
    }

    return 0;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int operands = argc - 1;
    if (operands < 1) {
        printf("mv: missing file operand\n");
        return 1;
    }

    if (operands < 2) {
        printf("mv: missing destination file operand after '%s'\n", argv[1]);
        return 1;
    }

    const char* dst = argv[argc - 1];
    struct stat st;
    int dst_is_dir = stat(dst, &st) == 0 && S_ISDIR(st.st_mode);
    if (operands > 2 && !dst_is_dir) {
        printf("mv: target '%s' is not a directory\n", dst);
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc - 1; i++) {
        rc |= move_operand(argv[i], dst, dst_is_dir);
    }

    return rc;
}
