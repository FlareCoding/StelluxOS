#ifndef STELLUX_TESTS_GRAPHICS_NVIDIA_COPY_CHANNEL_TEST_ACCESS_H
#define STELLUX_TESTS_GRAPHICS_NVIDIA_COPY_CHANNEL_TEST_ACCESS_H

#include "drivers/graphics/nvidia/copy_channel.h"
#include "clock/clock.h"
#include "hw/mmio.h"

namespace drivers::nvidia {

// Reads the copy channel's push buffer and channel as the GPU would, writes its semaphore as a release would, sets
// its sequence numbers and fill timeout, reads and sets its deadline, and reads whether it is hung
struct copy_channel_test_access {
    // The words of a push buffer slot, and where the semaphore lies in the channel's memory
    static constexpr uint32_t SLOT_WORDS       = 64;
    static constexpr uint64_t SEMAPHORE_OFFSET = 0x1000;

    static uint32_t slot_word(const copy_channel& copier, uint32_t slot, uint32_t word) {
        return mmio::read32(copier.m_memory.virt + (slot * SLOT_WORDS + word) * sizeof(uint32_t));
    }

    static void release(copy_channel& copier, uint32_t payload) {
        mmio::write32(copier.m_memory.virt + SEMAPHORE_OFFSET, payload);
    }

    static uint64_t push_buffer_phys(const copy_channel& copier) { return copier.m_memory.phys; }

    // Counts fills up to `submitted` as submitted, the oldest running one starting its deadline now
    static void set_submitted(copy_channel& copier, uint64_t submitted) {
        copier.m_submitted = submitted;
        copier.m_deadline_ns = clock::now_ns() + copier.m_fill_timeout_ns;
    }

    static void set_fill_timeout(copy_channel& copier, uint64_t timeout_ns) { copier.m_fill_timeout_ns = timeout_ns; }

    static void set_deadline(copy_channel& copier, uint64_t deadline_ns) { copier.m_deadline_ns = deadline_ns; }

    static uint64_t deadline(const copy_channel& copier) { return copier.m_deadline_ns; }

    static bool is_hung(const copy_channel& copier) { return copier.m_is_hung; }

    static const channel& queue(const copy_channel& copier) { return copier.m_channel; }
};

} // namespace drivers::nvidia

#endif // STELLUX_TESTS_GRAPHICS_NVIDIA_COPY_CHANNEL_TEST_ACCESS_H
