#include <stdio.h>

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("rm: missing operand\r\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (remove(argv[i]) < 0) {
            printf("rm: cannot remove '%s'\r\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
