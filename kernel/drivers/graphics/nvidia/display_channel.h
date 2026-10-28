// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_CHANNEL_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_CHANNEL_H

#include "common/types.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

// NVIDIA's size of the push buffer holding a display channel's queue of methods (writes to its class's registers), and
// of the pad at its end, which NVIDIA's driver leaves unwritten on every chip to work around a bug of earlier chips
constexpr uint32_t PUSH_BUFFER_BYTES     = 0x1000;
constexpr uint32_t PUSH_BUFFER_PAD_BYTES = 48;

// PUT and GET, the first two words of a channel's area in BAR0, each a byte offset into the push buffer
constexpr uint32_t CHANNEL_PUT = 0x0;
constexpr uint32_t CHANNEL_GET = 0x4;

// The most values one method carries: from offset 0, its header word, its values and a word kept free for a jump back
// to offset 0 fit before the pad
constexpr uint32_t METHOD_VALUES_MAX = (PUSH_BUFFER_BYTES - PUSH_BUFFER_PAD_BYTES) / 4 - 2;

// The driver's bound on a wait for GET to reach PUT, 5 seconds, after which NVIDIA's driver logs a wait for room in a
// push buffer as an error and waits on
constexpr uint64_t IDLE_TIMEOUT_NS = 5000000000;

/**
 * A display channel is a queue of methods that the display engine (the GPU's
 * unit that scans images out to monitors) reads from memory. This object is
 * the driver's view of one display channel's push buffer and of the channel's
 * PUT and GET, the first two registers of its area in BAR0. Each method names
 * a register offset of the channel's class and carries the values written
 * there. The driver writes methods into the push buffer, then kicks: it writes
 * PUT, the byte offset up to which the display engine may read. The display
 * engine reads methods from GET, the byte offset it reads next, and the
 * channel is idle once GET reaches PUT. The written end is where the next word
 * goes. While GET lies at or behind it, the push buffer holds, from offset 0:
 *
 *   0                                                     writable end
 *   +----------+---------------+------------------+-------+------+
 *   | read     | kicked,       | written, not     | free  | pad  |
 *   |          | unread        | kicked yet       |       |      |
 *   +----------+---------------+------------------+-------+------+
 *              GET             PUT                written end    4 KiB
 *
 * The writable end lies `PUSH_BUFFER_PAD_BYTES` short of the push buffer's
 * end. As NVIDIA's driver does, the object keeps a count of the words known
 * free at the written end and writes a method there only while the count
 * exceeds the words the method takes. Once a method would take as many words
 * as the count, or more, the object reads GET and counts again. While GET lies
 * at or behind the written end, a read of GET counts to the writable end, so
 * every method ends at least a word short of it, keeping that word for a later
 * jump. A method that does not fit there wraps, as in NVIDIA's driver: once
 * GET has left offset 0, a jump to offset 0 goes at the written end, PUT is
 * kicked to 0 and the object counts again. While GET lies ahead of the written
 * end, a read of GET counts to a word short of GET, or counts none once the
 * written end lies within five words of GET (NVIDIA's chase pad). A method
 * written on the count of an earlier read may still start within the chase
 * pad, ending at least two words short of GET.
 *
 * The object owns neither the push buffer nor the channel's area: its creator
 * must keep both mapped for the object's life, and the push buffer's memory
 * allocated for as long as the display engine may read it. It takes PUT and
 * GET to read 0, the offset a channel's allocation gives them, and starts the
 * written end there, so it must be created before anything writes the
 * channel's PUT. Its calls must come from one context at a time, since it
 * takes no lock. Only `wait_idle` waits.
 */
class display_channel {
public:
    /**
     * @brief A channel whose push buffer lies at `push_buffer` and whose PUT
     * and GET are the first two words of `area`.
     * @param push_buffer The CPU's mapping of the channel's
     * `PUSH_BUFFER_BYTES` of push buffer, which the display engine must read
     * with snooping on, so that it sees every write ordered before a kick's
     * barrier.
     * @param area The channel's area in BAR0.
     */
    display_channel(uint32_t* push_buffer, registers area);

    display_channel(const display_channel&) = delete;
    display_channel& operator=(const display_channel&) = delete;

    /**
     * @brief Write a method at the written end as NVIDIA's driver writes one:
     * a header naming `method` and `count`, then the values. Never waits, and
     * kicks only when the method wraps.
     * @param method The method's register offset in the channel's class, a
     * multiple of 4 up to 0x3FFC, the most a header names.
     * @param values The `count` values, written to `method`'s register and the
     * registers after it in turn.
     * @param count How many values, 1 to `METHOD_VALUES_MAX`.
     * @return `OK`, `ERR_QUEUE_FULL` when the method does not fit before GET
     * or cannot wrap while GET is at offset 0, `ERR_INVALID_ARG` for a
     * `method` or `count` outside the ranges above, or `ERR_BAD_STATE` when a
     * read of GET finds it past the writable end or not a multiple of 4 (a
     * card that stops responding reads all ones). `ERR_QUEUE_FULL` is not
     * logged, as the caller acts on it: one that may wait must kick, wait with
     * `wait_idle` and write the method again, repeating the three while
     * `write` returns `ERR_QUEUE_FULL` and `wait_idle` returns `OK`, and one
     * that must not wait refuses its own request. Without the kick, GET may
     * already read PUT with the methods written after the last kick unread.
     * The three run at most twice, a second time only for a method of more
     * than 504 values whose write after the first wait wraps and finds GET
     * still at the jump, so each wait must take a deadline counted from its
     * own start. The other errors are logged. On every error the method is not
     * written, though a wrap before the error leaves its jump written and PUT
     * kicked to 0.
     */
    int32_t write(uint32_t method, const uint32_t* values, uint32_t count);

    /**
     * @brief Make PUT the written end, after a full barrier, so the display
     * engine finds every method written before the kick. Writes nothing while
     * the written end is the last PUT kicked.
     */
    void kick();

    /**
     * @brief Whether GET reads PUT, so the display engine has read every
     * kicked method.
     * @return True when GET reads PUT, false otherwise, including while GET
     * reads past the writable end or not a multiple of 4.
     */
    bool idle() const;

    /**
     * @brief Spin until GET reads PUT or `deadline_ns` passes, reading GET at
     * least once. It must not run under a spinlock or in an interrupt handler,
     * since it spins until the deadline.
     * @param deadline_ns A time as `clock::now_ns` reads it.
     * @return `OK`, `ERR_TIMEOUT` when the clock reads past the deadline after
     * a read finding GET not at PUT, or `ERR_BAD_STATE` when GET reads past
     * the writable end or not a multiple of 4, each but `OK` logged.
     */
    int32_t wait_idle(uint64_t deadline_ns);

private:
    // Reads GET into `get` for `step`, refusing a GET past the writable end or not a multiple of 4
    int32_t read_get(const char* step, uint32_t& get) const;

    // Counts the words free at the written end from GET, and again after a wrap when GET lies at or behind the written
    // end, not at offset 0, and a method of `words` would end at or past the writable end
    int32_t make_room(uint32_t words);

    // The words free at the written end by NVIDIA's rules, with GET at `get`
    uint32_t free_words(uint32_t get) const;

    // Writes the jump to offset 0 at the written end, kicks PUT to 0 and counts no word free until a write reads GET
    void wrap();

    uint32_t* m_push_buffer;
    registers m_area;
    uint32_t  m_end;        // the written end, where the next word goes
    uint32_t  m_put;        // the last PUT kicked
    uint32_t  m_free_words; // the words known free at the written end, which only a read of GET raises
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_CHANNEL_H
