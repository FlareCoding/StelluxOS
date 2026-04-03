#include "trap/trap.h"
#include "hw/barrier.h"
#include "common/types.h"

extern "C" char stlx_aarch64_vectors[];

namespace trap {

__PRIVILEGED_CODE int32_t init() {
    const uint64_t addr = reinterpret_cast<uint64_t>(stlx_aarch64_vectors);
    asm volatile("msr vbar_el1, %0" : : "r"(addr) : "memory");
    barrier::instruction();
    return OK;
}

__PRIVILEGED_CODE void load() {
    const uint64_t addr = reinterpret_cast<uint64_t>(stlx_aarch64_vectors);
    asm volatile("msr vbar_el1, %0" : : "r"(addr) : "memory");
    asm volatile("isb" ::: "memory");
}

} // namespace trap

