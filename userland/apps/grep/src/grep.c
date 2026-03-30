#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        write(2, "usage: grep PATTERN\n", 20);
        return 1;
    }

    const char* pattern = argv[1];
    char line[4096];

    while (fgets(line, sizeof(line), stdin)) {
        if (strstr(line, pattern)) {
            fputs(line, stdout);
        }
    }

    return 0;
}
