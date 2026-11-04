#include "drivers/graphics/nvidia/engines.h"
#include "drivers/graphics/nvidia/interrupts.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "hw/barrier.h"
#include "hw/mmio.h"

namespace drivers::nvidia {

// The kernel's copy channel's id, the lowest of its runlist, where no other channel of the driver runs
constexpr uint32_t COPY_CHANNEL_ID = 0;

// The video memory the check fills, followed by a page the fills must leave as the CPU wrote it
constexpr uint64_t CHECK_BYTES       = 64 * 1024;
constexpr uint64_t CHECK_GUARD_BYTES = SMALL_PAGE_BYTES;

// What the CPU writes through BAR1 before the copies, and what the second copy fills. Neither is 0 or all ones, which
// BAR1 reads where no page is mapped and on a card that stopped responding
constexpr uint32_t CHECK_CPU_PATTERN  = 0x5A5A5A5A;
constexpr uint32_t CHECK_FILL_PATTERN = 0xC3C3C3C3;

constexpr uint64_t BYTES_PER_KIB = 1024;

// Finds the first 4-byte word in [first, end) at `virt` reading other than `expected`, or returns `end`
static uint64_t find_differing_word(uintptr_t virt, uint64_t first, uint64_t end, uint32_t expected) {
    for (uint64_t i = first; i < end; i++) {
        if (mmio::read32(virt + i * sizeof(uint32_t)) != expected) {
            return i;
        }
    }

    return end;
}

engines::engines(firmware& fw, const gpu_objects& objects, video_memory& memory, failed_mark& failed)
    : m_firmware(fw), m_objects(objects), m_memory(memory), m_failed(failed) {}

int32_t engines::bring_up(registers& bar0, const dma::device_profile& card, const chip_info& chip) {
    int32_t rc = m_table.read(m_firmware, m_objects.subdevice);
    if (rc != OK) {
        return rc;
    }

    const engine_info* copy_engine = m_table.first_async_copy_engine();
    if (copy_engine == nullptr) {
        log::error(
            "nvidia: the device info table lists %u engines and no asynchronous copy engine, so the kernel has no "
            "copy channel",
            m_table.count()
        );
        return ERR_BAD_STATE;
    }

    uint32_t vector = 0;
    rc = read_copy_vector(m_firmware, copy_engine->type, chip.interrupt_leaf_count, vector);
    if (rc != OK) {
        return rc;
    }

    m_copy_channel = new (m_copy_channel_bytes) copy_channel(m_firmware, m_objects, m_memory, bar0, m_failed);
    rc = m_copy_channel->create(card, chip, *copy_engine, COPY_CHANNEL_ID);
    if (rc != OK) {
        log::error("nvidia: creating the kernel's copy channel failed: %d", rc);
        return rc;
    }

    // Published before the enable, since a release pending already raises the interrupt at once
    m_copy_vector = vector;
    m_is_copy_vector_on.store_release(true);
    enable_vector(bar0, vector);

    rc = check_copy_channel();
    if (rc == OK) {
        log::info(
            "nvidia: the kernel's copy channel runs as channel %u on runlist %u of copy engine type 0x%X, and its "
            "releases raise GPU interrupt vector %u, bit %u of the CPU interrupt tree's leaf %u",
            COPY_CHANNEL_ID,
            copy_engine->runlist,
            copy_engine->type,
            vector,
            vector % VECTORS_PER_LEAF,
            vector / VECTORS_PER_LEAF
        );
        return OK;
    }

    m_is_copy_vector_on.store_release(false);
    mask_vector(bar0, vector);
    if (rc != ERR_TIMEOUT) {
        m_copy_channel->destroy();
    }

    return rc;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void engines::service_interrupt(registers& bar0) {
    if (!m_is_copy_vector_on.load_acquire()) {
        return;
    }

    if (acknowledge_nonstall_interrupt(bar0, m_copy_vector)) {
        m_copy_channel->service();
    }
}

int32_t engines::check_copy_channel() {
    vram_range range = {};
    int32_t rc = m_memory.allocate(CHECK_BYTES + CHECK_GUARD_BYTES, SMALL_PAGE_BYTES, vram_use::general, range);
    if (rc != OK) {
        log::error("nvidia: allocating video memory for the copy channel's check failed: %d", rc);
        return rc;
    }

    cpu_mapping mapping = {};
    rc = m_memory.map_for_cpu(range, mapping);
    if (rc != OK) {
        log::error("nvidia: mapping the copy channel's check memory through BAR1 failed: %d", rc);
        m_memory.release(range);
        return rc;
    }

    uint64_t all_words = (CHECK_BYTES + CHECK_GUARD_BYTES) / sizeof(uint32_t);
    for (uint64_t i = 0; i < all_words; i++) {
        mmio::write32(mapping.virt + i * sizeof(uint32_t), CHECK_CPU_PATTERN);
    }

    // The read returns once the writes are in video memory, before any copy is submitted. On x86_64 a store fence
    // orders no read after the writes
    barrier::dma_full();
    (void)mmio::read32(mapping.virt + (all_words - 1) * sizeof(uint32_t));

    uint64_t zero_ns = 0;
    uint64_t fill_ns = 0;
    rc = fill_and_compare(range, mapping.virt, 0, zero_ns);
    if (rc == OK) {
        rc = fill_and_compare(range, mapping.virt, CHECK_FILL_PATTERN, fill_ns);
    }

    if (rc == ERR_TIMEOUT) {
        m_memory.unmap_for_cpu(range);
        log::error(
            "nvidia: the copy channel's check memory at 0x%lX in video memory stays allocated for good, since the hung "
            "copy may still write it",
            range.offset
        );
        return rc;
    }

    m_memory.release(range);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the kernel's copy channel zeroed %lu KiB of video memory in %lu us, then filled it with 0x%X in %lu "
        "us, each read back through BAR1 with the page after it unchanged",
        CHECK_BYTES / BYTES_PER_KIB,
        zero_ns / NS_PER_US,
        CHECK_FILL_PATTERN,
        fill_ns / NS_PER_US
    );
    return OK;
}

int32_t engines::fill_and_compare(const vram_range& range, uintptr_t virt, uint32_t value, uint64_t& took_ns) {
    uint64_t start_ns = clock::now_ns();
    int32_t rc = m_copy_channel->fill(range.offset, CHECK_BYTES, value);
    took_ns = clock::now_ns() - start_ns;
    if (rc != OK) {
        return rc;
    }

    // A read through the write-combining mapping could otherwise happen before the read that saw the copy finish
    barrier::dma_read();

    uint64_t filled_words = CHECK_BYTES / sizeof(uint32_t);
    uint64_t all_words = (CHECK_BYTES + CHECK_GUARD_BYTES) / sizeof(uint32_t);
    uint32_t expected = value;
    uint64_t differing = find_differing_word(virt, 0, filled_words, value);
    if (differing == filled_words) {
        expected = CHECK_CPU_PATTERN;
        differing = find_differing_word(virt, filled_words, all_words, expected);
    }

    if (differing != all_words) {
        log::error(
            "nvidia: after the kernel's copy channel filled 0x%X, word %lu of the check's memory at 0x%lX in video "
            "memory reads 0x%X through BAR1, where 0x%X is expected",
            value,
            differing,
            range.offset,
            mmio::read32(virt + differing * sizeof(uint32_t)),
            expected
        );
        return ERR_BAD_STATE;
    }

    return OK;
}

} // namespace drivers::nvidia
