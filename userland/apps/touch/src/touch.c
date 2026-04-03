#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("touch: missing operand\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            printf("touch: cannot create '%s'\n", argv[i]);
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}
