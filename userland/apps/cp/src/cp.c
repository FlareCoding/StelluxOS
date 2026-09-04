#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

// Recursion extends these two paths one component at a time and trims them
// on the way back, so stack use stays flat however deep the tree is
static char g_src[PATH_MAX];
static char g_dst[PATH_MAX];
static char g_buf[64 * 1024];
static int g_recursive;

static int copy_node(void);

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

static int same_file(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

// A trailing slash promises the destination is a directory
static int wants_dir(const char* path) {
    size_t len = strlen(path);
    return len > 1 && path[len - 1] == '/';
}

// Appends one component, returning the old length so the caller can trim
// it again, or -1 when the result would not fit
static int path_push(char* path, const char* name) {
    size_t len = strlen(path);
    if (len + 1 + strlen(name) >= PATH_MAX) {
        return -1;
    }

    size_t old = len;
    if (len > 0 && path[len - 1] != '/') {
        path[len++] = '/';
    }
    strcpy(path + len, name);

    return (int)old;
}

// True when dir is the directory that will hold path or any ancestor of it,
// which is when a recursive copy would descend into its own output forever
static int dir_contains(const struct stat* dir, const char* path) {
    char cur[PATH_MAX];
    const char* slash = strrchr(path, '/');
    if (!slash) {
        strcpy(cur, ".");
    } else {
        size_t len = slash == path ? 1 : (size_t)(slash - path);
        memcpy(cur, path, len);
        cur[len] = '\0';
    }

    struct stat st;
    struct stat prev = {0};
    for (;;) {
        if (stat(cur, &st) < 0) {
            return 0;
        }

        if (same_file(dir, &st)) {
            return 1;
        }

        if (same_file(&st, &prev) || strlen(cur) + 3 >= PATH_MAX) {
            return 0;
        }

        prev = st;
        strcat(cur, "/..");
    }
}

static int copy_file(const struct stat* src_st) {
    struct stat dst_st;
    if (stat(g_dst, &dst_st) == 0) {
        if (S_ISDIR(dst_st.st_mode)) {
            printf("cp: cannot overwrite directory '%s' with non-directory\n", g_dst);
            return 1;
        }

        if (same_file(src_st, &dst_st)) {
            printf("cp: '%s' and '%s' are the same file\n", g_src, g_dst);
            return 1;
        }
    }

    int in = open(g_src, O_RDONLY);
    if (in < 0) {
        printf("cp: cannot open '%s': %s\n", g_src, strerror(errno));
        return 1;
    }

    int out = open(g_dst, O_WRONLY | O_CREAT | O_TRUNC, src_st->st_mode & 0777);
    if (out < 0) {
        printf("cp: cannot create '%s': %s\n", g_dst, strerror(errno));
        close(in);
        return 1;
    }

    int rc = 0;
    for (;;) {
        ssize_t n = read(in, g_buf, sizeof(g_buf));
        if (n == 0) {
            break;
        }

        if (n < 0) {
            printf("cp: error reading '%s': %s\n", g_src, strerror(errno));
            rc = 1;
            break;
        }

        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, g_buf + off, (size_t)(n - off));
            if (w < 0) {
                printf("cp: error writing '%s': %s\n", g_dst, strerror(errno));
                rc = 1;
                break;
            }
            off += w;
        }

        if (rc) {
            break;
        }
    }

    close(in);
    close(out);
    return rc;
}

static int copy_symlink(void) {
    char target[PATH_MAX];
    ssize_t len = readlink(g_src, target, sizeof(target) - 1);
    if (len < 0) {
        printf("cp: cannot read link '%s': %s\n", g_src, strerror(errno));
        return 1;
    }
    target[len] = '\0';

    if (symlink(target, g_dst) < 0) {
        printf("cp: cannot create symbolic link '%s': %s\n", g_dst, strerror(errno));
        return 1;
    }

    return 0;
}

static int copy_dir(const struct stat* src_st) {
    struct stat dst_st;
    if (stat(g_dst, &dst_st) == 0) {
        if (!S_ISDIR(dst_st.st_mode)) {
            printf("cp: cannot overwrite non-directory '%s' with directory '%s'\n", g_dst, g_src);
            return 1;
        }
    } else if (mkdir(g_dst, src_st->st_mode & 0777) < 0) {
        printf("cp: cannot create directory '%s': %s\n", g_dst, strerror(errno));
        return 1;
    }

    DIR* dir = opendir(g_src);
    if (!dir) {
        printf("cp: cannot open directory '%s': %s\n", g_src, strerror(errno));
        return 1;
    }

    int rc = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        int src_len = path_push(g_src, ent->d_name);
        int dst_len = src_len < 0 ? -1 : path_push(g_dst, ent->d_name);
        if (dst_len < 0) {
            printf("cp: path too long for '%s' under '%s'\n", ent->d_name, g_dst);
            rc = 1;
        } else {
            rc |= copy_node();
            g_dst[dst_len] = '\0';
        }

        if (src_len >= 0) {
            g_src[src_len] = '\0';
        }
    }

    closedir(dir);
    return rc;
}

// Entries below the top level keep their own identity: links stay links
static int copy_node(void) {
    struct stat st;
    if (lstat(g_src, &st) < 0) {
        printf("cp: cannot stat '%s': %s\n", g_src, strerror(errno));
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        return copy_dir(&st);
    }

    if (S_ISLNK(st.st_mode)) {
        return copy_symlink();
    }

    if (S_ISREG(st.st_mode)) {
        return copy_file(&st);
    }

    printf("cp: cannot copy '%s': not a regular file\n", g_src);
    return 1;
}

static int copy_operand(const char* src, const char* dst, int dst_is_dir) {
    if (strlen(src) >= PATH_MAX || strlen(dst) >= PATH_MAX) {
        printf("cp: path too long: '%s'\n", strlen(src) >= PATH_MAX ? src : dst);
        return 1;
    }

    strcpy(g_src, src);
    strcpy(g_dst, dst);
    strip_trailing_slashes(g_src);
    strip_trailing_slashes(g_dst);

    struct stat st;
    if (stat(g_src, &st) < 0) {
        printf("cp: cannot stat '%s': %s\n", g_src, strerror(errno));
        return 1;
    }

    if (dst_is_dir && path_push(g_dst, base_name(g_src)) < 0) {
        printf("cp: path too long for '%s' under '%s'\n", base_name(g_src), g_dst);
        return 1;
    }

    if (!dst_is_dir && wants_dir(dst) && !S_ISDIR(st.st_mode)) {
        printf("cp: cannot create '%s': Not a directory\n", dst);
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!g_recursive) {
            printf("cp: -r not specified; omitting directory '%s'\n", g_src);
            return 1;
        }

        if (dir_contains(&st, g_dst)) {
            printf("cp: cannot copy a directory, '%s', into itself, '%s'\n", g_src, g_dst);
            return 1;
        }

        return copy_dir(&st);
    }

    if (S_ISREG(st.st_mode)) {
        return copy_file(&st);
    }

    printf("cp: cannot copy '%s': not a regular file\n", g_src);
    return 1;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (int j = 1; argv[first][j]; j++) {
            switch (argv[first][j]) {
            case 'r':
            case 'R':
                g_recursive = 1;
                break;
            default:
                printf("cp: unknown option '-%c'\n", argv[first][j]);
                return 1;
            }
        }
    }

    int operands = argc - first;
    if (operands < 1) {
        printf("cp: missing file operand\n");
        return 1;
    }

    if (operands < 2) {
        printf("cp: missing destination file operand after '%s'\n", argv[first]);
        return 1;
    }

    const char* dst = argv[argc - 1];
    struct stat st;
    int dst_is_dir = stat(dst, &st) == 0 && S_ISDIR(st.st_mode);
    if (operands > 2 && !dst_is_dir) {
        printf("cp: target '%s' is not a directory\n", dst);
        return 1;
    }

    int rc = 0;
    for (int i = first; i < argc - 1; i++) {
        rc |= copy_operand(argv[i], dst, dst_is_dir);
    }

    return rc;
}
