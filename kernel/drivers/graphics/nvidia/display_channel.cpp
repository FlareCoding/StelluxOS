// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "hw/barrier.h"
#include "hw/cpu.h"

namespace drivers::nvidia {

// PUT and GET, the first two words of a channel's area in BAR0, each a byte offset into the push buffer
constexpr uint32_t CHANNEL_PUT = 0x0;
constexpr uint32_t CHANNEL_GET = 0x4;

// A method's header: opcode 0 in bits 31:29, the count of values in bits 27:18 and the register offset in bits 13:2
constexpr uint32_t METHOD_COUNT_SHIFT = 18;
constexpr uint32_t METHOD_OFFSET_MAX  = 0x3FFC;

// The jump a wrap writes: the jump opcode, 1, in bits 31:29 and offset 0
constexpr uint32_t JUMP_TO_START = 0x20000000;

// The pad's start, which every method ends short of, and NVIDIA's chase pad: while GET lies ahead of the written end,
// no word is counted free when GET is read with the written end within the chase pad of GET
constexpr uint32_t WRITABLE_END    = PUSH_BUFFER_BYTES - PUSH_BUFFER_PAD_BYTES;
constexpr uint32_t CHASE_PAD_BYTES = 5 * 4;

// NVIDIA's count of free words for a channel nothing has written to, two words short of the writable end
constexpr uint32_t FRESH_FREE_WORDS = WRITABLE_END / 4 - 2;

display_channel::display_channel(uint32_t* push_buffer, registers area)
    : m_push_buffer(push_buffer), m_area(area), m_end(0), m_put(0), m_free_words(FRESH_FREE_WORDS) {}

int32_t display_channel::write(uint32_t method, const uint32_t* values, uint32_t count) {
    if (method % 4 != 0) {
        log::error("nvidia: a display method's register offset 0x%X is not a multiple of 4", method);
        return ERR_INVALID_ARG;
    }

    if (method > METHOD_OFFSET_MAX) {
        log::error(
            "nvidia: a display method's register offset 0x%X lies past 0x%X, the most a method's header names",
            method,
            METHOD_OFFSET_MAX
        );
        return ERR_INVALID_ARG;
    }

    bool count_in_range = count >= 1 && count <= METHOD_VALUES_MAX;
    if (!count_in_range) {
        log::error(
            "nvidia: a display method for register offset 0x%X carries %u values, outside 1 to %u",
            method,
            count,
            METHOD_VALUES_MAX
        );
        return ERR_INVALID_ARG;
    }

    uint32_t words = count + 1;
    if (m_free_words <= words) {
        int32_t rc = make_room(words);
        if (rc != OK) {
            return rc;
        }
    }

    uint32_t* method_start = m_push_buffer + m_end / 4;
    method_start[0] = count << METHOD_COUNT_SHIFT | method;
    for (uint32_t i = 0; i < count; i++) {
        method_start[1 + i] = values[i];
    }

    m_end += words * 4;
    m_free_words -= words;

    return OK;
}

void display_channel::kick() {
    if (m_put == m_end) {
        return;
    }

    barrier::io_full();
    m_area.write(CHANNEL_PUT, m_end);
    m_put = m_end;
}

bool display_channel::idle() const {
    return m_area.read(CHANNEL_GET) == m_put;
}

int32_t display_channel::wait_idle(uint64_t deadline_ns) {
    while (true) {
        uint32_t get = 0;
        int32_t rc = read_get("while waiting for GET to reach PUT", get);
        if (rc != OK) {
            return rc;
        }

        if (get == m_put) {
            return OK;
        }

        if (clock::now_ns() > deadline_ns) {
            log::error("nvidia: a display channel's GET 0x%X did not reach PUT 0x%X by the deadline", get, m_put);
            return ERR_TIMEOUT;
        }

        cpu::relax();
    }
}

int32_t display_channel::read_get(const char* step, uint32_t& get) const {
    uint32_t value = m_area.read(CHANNEL_GET);
    if (value > WRITABLE_END) {
        log::error(
            "nvidia: a display channel's GET 0x%X, read %s, lies past its push buffer's writable end 0x%X",
            value,
            step,
            WRITABLE_END
        );
        return ERR_BAD_STATE;
    }

    if (value % 4 != 0) {
        log::error("nvidia: a display channel's GET 0x%X, read %s, is not a multiple of 4", value, step);
        return ERR_BAD_STATE;
    }

    get = value;

    return OK;
}

// NVIDIA's count without its wait for room, passing twice after a wrap: a method needs more words free than it takes
int32_t display_channel::make_room(uint32_t words) {
    uint32_t get = 0;
    int32_t rc = read_get("to count its push buffer's free words", get);
    if (rc != OK) {
        return rc;
    }

    m_free_words = free_words(get);
    bool needs_wrap = m_end >= get && m_free_words <= words;
    if (needs_wrap) {
        if (get == 0) {
            return ERR_QUEUE_FULL;
        }

        wrap();
        rc = read_get("after its push buffer wrapped to offset 0", get);
        if (rc != OK) {
            return rc;
        }

        m_free_words = free_words(get);
    }

    if (m_free_words <= words) {
        return ERR_QUEUE_FULL;
    }

    return OK;
}

// The count a read of GET gives: while GET lies at or behind the written end, the words to the writable end, and while
// it lies ahead, the words up to a word short of GET, or none with the written end within the chase pad of GET
uint32_t display_channel::free_words(uint32_t get) const {
    if (m_end >= get) {
        return (WRITABLE_END - m_end) / 4;
    }

    if (m_end + CHASE_PAD_BYTES >= get) {
        return 0;
    }

    return (get - m_end) / 4 - 1;
}

void display_channel::wrap() {
    m_push_buffer[m_end / 4] = JUMP_TO_START;
    m_end = 0;
    m_free_words = 0;
    kick();
}

} // namespace drivers::nvidia
