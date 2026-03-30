#include <stdio.h>
#include <unistd.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    int c;

    while ((c = getchar()) != EOF) {
        bytes++;
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }

    printf("  %d %d %d\n", lines, words, bytes);
    return 0;
}
