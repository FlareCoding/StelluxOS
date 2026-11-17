#include "Arduino.h"

#include <stdio.h>
#include <time.h>

void print_line(const char* text) {
    puts(text);
}

unsigned long board_millis(void) {
    return (unsigned long)(clock() / (CLOCKS_PER_SEC / 1000));
}
