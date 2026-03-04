#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#define COL_RESET   "\x1b[0m"
#define COL_DIR     "\x1b[1;34m"
#define COL_CHR     "\x1b[1;33m"
#define COL_SOCK    "\x1b[1;35m"

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

static const char* color_for_mode(mode_t mode) {
    if (S_ISDIR(mode))  return COL_DIR;
    if (S_ISCHR(mode))  return COL_CHR;
    if (S_ISSOCK(mode)) return COL_SOCK;
    return NULL;
}

static int list_dir(const char* path, int flag_long, int flag_all) {
    DIR* dir = opendir(path);
    if (!dir) {
        printf("ls: cannot open '%s'\r\n", path);
        return 1;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!flag_all && ent->d_name[0] == '.') continue;

        char full_path[512];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);
        if (n < 0 || n >= (int)sizeof(full_path)) continue;

        struct stat st;
        int have_stat = (stat(full_path, &st) == 0);

        const char* color = have_stat ? color_for_mode(st.st_mode) : NULL;
        int is_dir = have_stat && S_ISDIR(st.st_mode);

        if (flag_long) {
            char mode_str[11] = "??????????";
            long long size = -1;
            if (have_stat) {
                format_mode(st.st_mode, mode_str);
                size = (long long)st.st_size;
            }
            if (color) {
                printf("  %s %8lld %s%s%s%s\r\n",
                       mode_str, size, color, ent->d_name,
                       is_dir ? "/" : "", COL_RESET);
            } else {
                printf("  %s %8lld %s%s\r\n",
                       mode_str, size, ent->d_name,
                       is_dir ? "/" : "");
            }
        } else {
            if (color) {
                printf("%s%s%s%s\r\n", color, ent->d_name,
                       is_dir ? "/" : "", COL_RESET);
            } else {
                printf("%s%s\r\n", ent->d_name, is_dir ? "/" : "");
            }
        }
    }

    closedir(dir);
    return 0;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int flag_long = 0;
    int flag_all = 0;
    const char* path = ".";

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                switch (argv[i][j]) {
                case 'l': flag_long = 1; break;
                case 'a': flag_all = 1; break;
                default:
                    printf("ls: unknown option '-%c'\r\n", argv[i][j]);
                    return 1;
                }
            }
        } else {
            path = argv[i];
        }
    }

    return list_dir(path, flag_long, flag_all);
}
