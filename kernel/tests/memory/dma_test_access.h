#ifndef STELLUX_TESTS_MEMORY_DMA_TEST_ACCESS_H
#define STELLUX_TESTS_MEMORY_DMA_TEST_ACCESS_H

#include "dma/dma.h"

namespace dma {

// Builds any profile, coherent ones included, which outside tests only the bus layer may build
struct device_profile_test_access {
    static device_profile build(uint8_t address_bits, bool coherent) {
        return device_profile(address_bits, coherent);
    }
};

} // namespace dma

#endif // STELLUX_TESTS_MEMORY_DMA_TEST_ACCESS_H
