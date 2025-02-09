#include "test.h"
#include <serial/serial.h>

void test_fn(int x) {
    serial::printf("x is %i\n", x);
}
