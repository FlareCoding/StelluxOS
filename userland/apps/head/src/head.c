#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int n = 10;
    if (argc >= 2) {
        n = atoi(argv[1]);
        if (n <= 0) n = 10;
    }

    char line[4096];
    int count = 0;

    while (count < n && fgets(line, sizeof(line), stdin)) {
        fputs(line, stdout);
        count++;
    }

    return 0;
}
