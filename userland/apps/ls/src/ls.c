#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define COL_RESET   "\x1b[0m"
#define COL_DIR     "\x1b[1;34m"
#define COL_CHR     "\x1b[1;33m"
#define COL_SOCK    "\x1b[1;35m"

#define TERM_WIDTH  80
#define MAX_ENTRIES 512
#define NAME_LEN    256

struct ls_entry {
    char        name[NAME_LEN];
    const char* color;
    int         is_dir;
    mode_t      mode;
    long long   size;
    unsigned long long ino;
    int         have_stat;
};

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

static int collect_entries(const char* path, int flag_all,
                           struct ls_entry* entries, int* out_count,
                           int* out_max_name_len) {
    DIR* dir = opendir(path);
    if (!dir) {
        printf("ls: cannot open '%s'\n", path);
        return 1;
    }

    int count = 0;
    int max_len = 0;
    struct dirent* ent;

    while ((ent = readdir(dir)) != NULL && count < MAX_ENTRIES) {
        if (!flag_all && ent->d_name[0] == '.') continue;

        struct ls_entry* e = &entries[count];
        strncpy(e->name, ent->d_name, NAME_LEN - 1);
        e->name[NAME_LEN - 1] = '\0';

        char full_path[512];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);
        if (n < 0 || n >= (int)sizeof(full_path)) {
            e->have_stat = 0;
            e->color = NULL;
            e->is_dir = 0;
            count++;
            continue;
        }

        struct stat st;
        e->have_stat = (stat(full_path, &st) == 0);
        if (e->have_stat) {
            e->color = color_for_mode(st.st_mode);
            e->is_dir = S_ISDIR(st.st_mode);
            e->mode = st.st_mode;
            e->size = (long long)st.st_size;
            e->ino = (unsigned long long)st.st_ino;
        } else {
            e->color = NULL;
            e->is_dir = 0;
            e->mode = 0;
            e->size = -1;
            e->ino = 0;
        }

        int display_len = (int)strlen(e->name) + (e->is_dir ? 1 : 0);
        if (display_len > max_len) max_len = display_len;

        count++;
    }

    closedir(dir);
    *out_count = count;
    *out_max_name_len = max_len;
    return 0;
}

static void print_entry_name(struct ls_entry* e) {
    const char* suffix = e->is_dir ? "/" : "";
    if (e->color) {
        printf("%s%s%s%s", e->color, e->name, suffix, COL_RESET);
    } else {
        printf("%s%s", e->name, suffix);
    }
}

static void print_columns(struct ls_entry* entries, int count, int max_display_len) {
    int col_width = max_display_len + 2;
    if (col_width < 4) col_width = 4;
    int num_cols = TERM_WIDTH / col_width;
    if (num_cols < 1) num_cols = 1;

    for (int i = 0; i < count; i++) {
        int display_len = (int)strlen(entries[i].name) + (entries[i].is_dir ? 1 : 0);
        int last_in_row = ((i + 1) % num_cols == 0) || (i == count - 1);

        print_entry_name(&entries[i]);

        if (last_in_row) {
            printf("\n");
        } else {
            int padding = col_width - display_len;
            for (int p = 0; p < padding; p++) putchar(' ');
        }
    }
}

static void print_long(struct ls_entry* entries, int count) {
    for (int i = 0; i < count; i++) {
        struct ls_entry* e = &entries[i];
        char mode_str[11] = "??????????";
        if (e->have_stat) format_mode(e->mode, mode_str);

        if (e->color) {
            printf("  %s %8lld %s%s%s%s\n",
                   mode_str, e->size, e->color, e->name,
                   e->is_dir ? "/" : "", COL_RESET);
        } else {
            printf("  %s %8lld %s%s\n",
                   mode_str, e->size, e->name,
                   e->is_dir ? "/" : "");
        }
    }
}

static int list_dir(const char* path, int flag_long, int flag_all) {
    struct ls_entry* entries = malloc(MAX_ENTRIES * sizeof(struct ls_entry));
    if (!entries) {
        printf("ls: out of memory\n");
        return 1;
    }

    int count = 0;
    int max_display_len = 0;
    int rc = collect_entries(path, flag_all, entries, &count, &max_display_len);
    if (rc != 0) {
        free(entries);
        return rc;
    }

    if (flag_long) {
        print_long(entries, count);
    } else {
        print_columns(entries, count, max_display_len);
    }

    free(entries);
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
                    printf("ls: unknown option '-%c'\n", argv[i][j]);
                    return 1;
                }
            }
        } else {
            path = argv[i];
        }
    }

    return list_dir(path, flag_long, flag_all);
}
