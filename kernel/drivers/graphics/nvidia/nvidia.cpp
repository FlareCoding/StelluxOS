#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "hw/cpu.h"

namespace drivers::nvidia {

void spin_for_ms(uint64_t ms) {
    uint64_t end = clock::now_ns() + ms * NS_PER_MS;
    while (clock::now_ns() < end) {
        cpu::relax();
    }
}

} // namespace drivers::nvidia
