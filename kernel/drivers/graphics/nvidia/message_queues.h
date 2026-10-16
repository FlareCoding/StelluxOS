// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_MESSAGE_QUEUES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_MESSAGE_QUEUES_H

#include "common/types.h"

namespace drivers::nvidia {

// Bytes of the memory the message queues take, their page table included
constexpr uint32_t QUEUE_MEMORY_BYTES = 0x81000;

// Where the command queue and the status queue start in that memory, and how many pages its page table lists: every
// page of the memory, its own included
constexpr uint32_t COMMAND_QUEUE_OFFSET     = 0x1000;
constexpr uint32_t STATUS_QUEUE_OFFSET      = 0x41000;
constexpr uint32_t QUEUE_PAGE_TABLE_ENTRIES = QUEUE_MEMORY_BYTES / 0x1000;

// The most payload bytes one message carries: 16 elements of 4 KiB less the message's 80 bytes of headers
constexpr uint32_t MESSAGE_PAYLOAD_MAX_BYTES = 16 * 0x1000 - 80;

// What `message_queues::receive` reports of a message the firmware wrote
struct received_message {
    uint32_t function;      // NVIDIA's number for what the message reports or answers
    uint32_t result;        // the status code the firmware put in the RPC header, its result for a request it answers
    uint32_t payload_bytes; // how many bytes the message carries after its headers
};

/**
 * The message queues the driver and the GSP's firmware talk through, two
 * rings in one block of memory the card reaches. The command queue carries
 * the driver's messages to the firmware, and the status queue carries the
 * firmware's replies and its unrequested messages back. The block starts
 * with a page table, which lists the address the card reaches each of the
 * block's 4 KiB pages at, so the firmware can reach the whole block from the
 * page table's address:
 *
 *   +---------------+ 0
 *   | page table    |
 *   +---------------+ 0x1000
 *   | command queue |  written by the driver, read by the firmware
 *   +---------------+ 0x41000
 *   | status queue  |  written by the firmware, read by the driver
 *   +---------------+ 0x81000
 *
 * Each queue starts with a header, in which the queue's writer records the
 * queue's layout and its write pointer, the index of the element its next
 * message starts in. A ring of 4 KiB elements follows. The command queue's
 * elements start after its 4 KiB header page, and the status queue's header
 * gives where its first element and the read pointer it holds lie. A message
 * fills one or more elements in a row, wrapping from the last element to the
 * first, and the writer moves its write pointer past the message only once
 * every byte of it is written. The reader keeps a read pointer, the index of
 * the element it reads next, and the writer fills only elements the reader
 * has passed, leaving one free so that a full ring differs from an empty one.
 * Each reader keeps its read pointer before the first element of the queue
 * it reads, or of the queue it writes when both queues' headers ask to swap,
 * so that neither side then writes to the queue it reads.
 * A message starts with an element header, which holds a checksum of the
 * message, its sequence number among the queue's messages and the number of
 * elements it fills, followed by NVIDIA's RPC header, which says what the
 * message asks or reports and how many bytes the RPC header and the payload
 * take.
 *
 * The driver writes the command queue's header before the firmware starts,
 * and the firmware writes the status queue's header once it runs. The driver
 * then links the status queue: it checks that header and learns from it
 * where the status queue's elements and each read pointer lie.
 *
 * The object keeps the driver's side of both queues: the command queue's
 * write pointer and next sequence number, and once the status queue is
 * linked, its read pointer and the sequence number its next message must
 * carry. It owns nothing, so the memory must stay allocated for as long as
 * the object is used. Its calls never wait, and only one thread may use it
 * at a time.
 */
class message_queues {
public:
    /**
     * @brief Lay the queue memory out as NVIDIA's driver does before the
     * firmware starts: zero the memory, write the page table and write the
     * command queue's header. The firmware writes the status queue's header.
     * @param memory The CPU's mapping of `QUEUE_MEMORY_BYTES` bytes. It must
     * be aligned to 8 bytes, since the page table is written in 8-byte
     * entries, and be memory the CPU may access at any alignment, since
     * messages are copied in at any alignment. It must be mapped so the card
     * sees the CPU's writes and the CPU sees the card's without cache
     * maintenance.
     * @param dma_address The address the card reaches the memory's first byte
     * at. It must be a multiple of 4 KiB, since the page table lists whole
     * pages, and the card must reach the whole memory as one contiguous range
     * from there, since the page table lists each page at `dma_address` plus
     * the page's offset. The memory must stay allocated for as long as the
     * card may reach it, which can outlast the object.
     */
    message_queues(uint8_t* memory, uint64_t dma_address);

    /**
     * @brief Write a message to the command queue, numbered with the next
     * sequence number, and move the write pointer past it. The firmware's read
     * pointer is read from the command queue's header page, where NVIDIA's
     * driver reads it before the status queue is linked, and from where the
     * status queue's header places it once the link finds that both queues'
     * headers ask to swap. A firmware that swaps keeps its read pointer there
     * once it has written the status queue's header, so before the link
     * `send` may see no element freed and return `ERR_QUEUE_FULL` once the
     * ring's free elements are used up. `send` touches none of the card's
     * registers, so after `OK` the caller must tell the firmware that a
     * message waits.
     * @param function NVIDIA's number for what the message asks of the
     * firmware.
     * @param payload The bytes the message carries after its headers, which
     * may be null when `payload_bytes` is 0.
     * @param payload_bytes How many bytes `payload` holds, refused above
     * `MESSAGE_PAYLOAD_MAX_BYTES`.
     * @return `OK`, `ERR_QUEUE_FULL` when the read pointer `send` reads for
     * the firmware lies past the ring or leaves too few elements free for the
     * message, or `ERR_TOO_LARGE`, which is logged, when `payload_bytes`
     * exceeds `MESSAGE_PAYLOAD_MAX_BYTES`. Either error writes nothing. A
     * retry of `ERR_QUEUE_FULL` succeeds only once that read pointer moves
     * on, which a firmware that swaps may show only after the link and one
     * that stops never shows, so the caller must bound its retries with a
     * deadline.
     */
    int32_t send(uint32_t function, const void* payload, uint32_t payload_bytes);

    /**
     * @brief Link the status queue once the firmware runs: check the header
     * the firmware wrote at the start of the status queue, then set the
     * driver's read pointer to 0 where NVIDIA's driver sets it, in the
     * command queue's header page when both queues' headers ask to swap and
     * where the status queue's header places it otherwise. From then on
     * `send` reads the firmware's read pointer where the status queue's
     * header places it when both ask to swap. A call after `OK` changes
     * nothing.
     * @return `OK`, or `ERR_NOT_LINKED` when the header gives another version,
     * queue size or element size than NVIDIA's, a first element ending past
     * the queue, an element count other than the whole elements that fit from
     * it to the queue's end, or a read pointer anywhere but between the header
     * and the first element, aligned to 4 bytes. Before the firmware writes
     * the header, its zeros fail these checks. The error writes nothing. A
     * retry returns `OK` once the firmware has written a header that passes,
     * which a firmware that stops first or lays the queue out otherwise never
     * does, so the caller must bound its retries with a deadline.
     */
    int32_t link_status_queue();

    /**
     * @brief Copy the payload of the next message in the status queue into
     * `payload` and move the read pointer past the message, once it passes
     * the checks NVIDIA's driver makes and two stricter ones. NVIDIA's driver
     * checks that the write pointer has passed every element the message
     * gives, that its RPC header gives 32 bytes to 64 KiB less the element
     * header, its checksum, and its sequence number, which must count the
     * status queue's messages from 0. A message numbered below the one due is
     * refused in place too, where NVIDIA's driver skips it. The message must
     * also give 1 to 16 elements, so its payload fits
     * `MESSAGE_PAYLOAD_MAX_BYTES`, and the length its RPC header gives must
     * fit them, so the checksum and the copy stay inside its own elements.
     * The headers are copied once and checked in that copy, and the payload
     * is summed and copied where it lies, while NVIDIA's driver copies the
     * whole message first. Both drivers see the same bytes, as the firmware
     * rewrites only elements the read pointer has passed.
     * @param out Receives the message's function, result and payload size,
     * which never exceeds `payload_capacity` or `MESSAGE_PAYLOAD_MAX_BYTES`,
     * on `OK`, and is left unchanged on an error.
     * @param payload Where the payload is copied, which may be null when
     * `payload_capacity` is 0. Bytes past the payload are left as they were.
     * @param payload_capacity How many bytes `payload` holds. A capacity of
     * `MESSAGE_PAYLOAD_MAX_BYTES` takes any message.
     * @return `OK`, `ERR_NOT_LINKED` when no call to `link_status_queue` has
     * returned `OK`, `ERR_QUEUE_EMPTY` when the firmware's write pointer
     * equals the read pointer or lies past the ring, `ERR_BAD_MESSAGE` when
     * the next message fails a check, or `ERR_TOO_LARGE` when its payload
     * exceeds `payload_capacity`, the last two logged. Every error leaves the
     * message in place and writes nothing, so a later call checks the same
     * message again. A caller retrying `ERR_QUEUE_EMPTY` or `ERR_BAD_MESSAGE`
     * must bound its retries with a deadline, as a firmware that stops writes
     * no message and a message that fails a check after the firmware wrote it
     * whole never passes.
     */
    int32_t receive(received_message& out, void* payload, uint32_t payload_capacity);

private:
    uint8_t*  m_command_queue;
    uint32_t* m_firmware_read_pointer;
    uint32_t  m_write_pointer;
    uint32_t  m_sequence_number;

    uint8_t*  m_status_queue;
    bool      m_is_status_queue_linked;
    uint8_t*  m_status_elements;
    uint32_t  m_status_element_count;
    uint32_t* m_driver_read_pointer;
    uint32_t  m_status_read_pointer;
    uint32_t  m_status_sequence_number;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_MESSAGE_QUEUES_H
