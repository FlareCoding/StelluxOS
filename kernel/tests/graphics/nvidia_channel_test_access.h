#ifndef STELLUX_TESTS_GRAPHICS_NVIDIA_CHANNEL_TEST_ACCESS_H
#define STELLUX_TESTS_GRAPHICS_NVIDIA_CHANNEL_TEST_ACCESS_H

#include "drivers/graphics/nvidia/channel.h"
#include "hw/mmio.h"

namespace drivers::nvidia {

// Reads the channel's GPFIFO and USERD as the GPU would, and sets its next slot
struct channel_test_access {
    static uint32_t gpfifo_word(const channel& queue, uint32_t slot, uint32_t word) {
        return mmio::read32(queue.m_gpfifo.virt + slot * GPFIFO_ENTRY_BYTES + word * sizeof(uint32_t));
    }

    static uint32_t userd_word(const channel& queue, uint32_t offset) {
        return mmio::read32(queue.m_userd.virt + offset);
    }

    static void set_put(channel& queue, uint32_t put) { queue.m_put = put; }
};

} // namespace drivers::nvidia

#endif // STELLUX_TESTS_GRAPHICS_NVIDIA_CHANNEL_TEST_ACCESS_H
