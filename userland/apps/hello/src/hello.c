#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[]) {
    int count = 1;
    if (argc > 1) {
        count = atoi(argv[1]);
        if (count <= 0) count = 1;
    }

    for (int i = 0; i < count; i++) {
        printf("hello\r\n");
    }

    return count;
}
