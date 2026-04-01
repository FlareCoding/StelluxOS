#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#define FLAG_S (1 << 0)
#define FLAG_N (1 << 1)
#define FLAG_R (1 << 2)
#define FLAG_V (1 << 3)
#define FLAG_M (1 << 4)
#define FLAG_ALL (FLAG_S | FLAG_N | FLAG_R | FLAG_V | FLAG_M)

static void usage(void) {
    printf("Usage: uname [OPTION]...\n"
           "Print system information.\n\n"
           "  -a   all information\n"
           "  -s   kernel name\n"
           "  -n   network node hostname\n"
           "  -r   kernel release\n"
           "  -v   kernel version\n"
           "  -m   machine hardware name\n"
           "  --help display this help\n");
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    unsigned flags = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            printf("uname: invalid option '%s'\n", argv[i]);
            return 1;
        }
        for (const char* p = argv[i] + 1; *p; p++) {
            switch (*p) {
            case 'a': flags |= FLAG_ALL; break;
            case 's': flags |= FLAG_S;   break;
            case 'n': flags |= FLAG_N;   break;
            case 'r': flags |= FLAG_R;   break;
            case 'v': flags |= FLAG_V;   break;
            case 'm': flags |= FLAG_M;   break;
            default:
                printf("uname: invalid option '-%c'\n", *p);
                return 1;
            }
        }
    }

    if (flags == 0)
        flags = FLAG_S;

    struct utsname buf;
    if (uname(&buf) != 0) {
        printf("uname: uname syscall failed\n");
        return 1;
    }

    int need_space = 0;
    const char* fields[] = { buf.sysname, buf.nodename, buf.release,
                             buf.version, buf.machine };
    unsigned field_flags[] = { FLAG_S, FLAG_N, FLAG_R, FLAG_V, FLAG_M };

    for (int i = 0; i < 5; i++) {
        if (flags & field_flags[i]) {
            if (need_space) putchar(' ');
            fputs(fields[i], stdout);
            need_space = 1;
        }
    }
    putchar('\n');

    return 0;
}
