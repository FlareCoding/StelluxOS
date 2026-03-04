#include <stdio.h>
#include <sys/stat.h>

static const char* file_type(mode_t mode) {
    if (S_ISREG(mode))  return "regular file";
    if (S_ISDIR(mode))  return "directory";
    if (S_ISCHR(mode))  return "character device";
    if (S_ISBLK(mode))  return "block device";
    if (S_ISLNK(mode))  return "symbolic link";
    if (S_ISSOCK(mode)) return "socket";
    if (S_ISFIFO(mode)) return "fifo";
    return "unknown";
}

static void format_mode(mode_t mode, char out[11]) {
    out[0] = S_ISDIR(mode)  ? 'd' :
             S_ISCHR(mode)  ? 'c' :
             S_ISBLK(mode)  ? 'b' :
             S_ISLNK(mode)  ? 'l' :
             S_ISSOCK(mode) ? 's' :
             S_ISFIFO(mode) ? 'p' :
             S_ISREG(mode)  ? '-' : '?';
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("stat: missing operand\r\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            printf("stat: cannot stat '%s'\r\n", argv[i]);
            rc = 1;
            continue;
        }

        char mode_str[11];
        format_mode(st.st_mode, mode_str);

        printf("  File: %s\r\n", argv[i]);
        printf("  Size: %lld\r\n", (long long)st.st_size);
        printf("  Type: %s\r\n", file_type(st.st_mode));
        printf("Access: %s (0%o)\r\n", mode_str, (unsigned)(st.st_mode & 0777));
        printf(" Inode: %llu\r\n", (unsigned long long)st.st_ino);
        if (i < argc - 1) printf("\r\n");
    }
    return rc;
}
