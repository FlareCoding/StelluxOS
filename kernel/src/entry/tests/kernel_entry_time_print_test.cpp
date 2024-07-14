#include "kernel_entry_tests.h"
#include <core/kprint.h>
#include <time/ktime.h>

void ke_test_print_current_time() {
    while (true) {
        sleep(1);
        kuPrint(
            "Ticked: ns:%llu us:%llu ms:%llu s:%llu\n",
            KernelTimer::getSystemTimeInNanoseconds(),
            KernelTimer::getSystemTimeInMicroseconds(),
            KernelTimer::getSystemTimeInMilliseconds(),
            KernelTimer::getSystemTimeInSeconds()
        );
    }
}
