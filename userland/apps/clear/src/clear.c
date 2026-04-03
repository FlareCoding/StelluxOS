#include <unistd.h>

int main(void) {
    write(1, "\x1b[2J\x1b[H", 7);
    return 0;
}
