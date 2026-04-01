#include <unistd.h>
#include <string.h>

int main(int argc, char* argv[]) {
    int trailing_newline = 1;
    int first_arg = 1;

    if (first_arg < argc && strcmp(argv[first_arg], "-n") == 0) {
        trailing_newline = 0;
        first_arg++;
    }

    for (int i = first_arg; i < argc; i++) {
        if (i > first_arg)
            write(1, " ", 1);
        write(1, argv[i], strlen(argv[i]));
    }

    if (trailing_newline)
        write(1, "\n", 1);

    return 0;
}
