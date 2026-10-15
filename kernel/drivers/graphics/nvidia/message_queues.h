// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_MESSAGE_QUEUES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_MESSAGE_QUEUES_H

#include "common/types.h"

namespace drivers::nvidia {

// Bytes of the memory the message queues take, their page table included
constexpr uint32_t QUEUE_MEMORY_BYTES = 0x81000;

// The most payload bytes one message carries: 16 elements of 4 KiB less the message's 80 bytes of headers
constexpr uint32_t MESSAGE_PAYLOAD_MAX_BYTES = 16 * 0x1000 - 80;

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
 * Each queue is a header page and a ring of 4 KiB elements. The queue's
 * writer records the queue's layout in the header page, and its write
 * pointer, the index of the element its next message starts in. A message
 * fills one or more elements in a row, wrapping from the last element to the
 * first, and the writer moves its write pointer past the message only once
 * every byte of it is written. The reader keeps a read pointer, the index of
 * the element it reads next, and the writer fills only elements the reader
 * has passed, leaving one free so that a full ring differs from an empty one.
 * A message starts with an element header, which holds a checksum of the
 * message, its sequence number among the queue's messages and the number of
 * elements it fills, followed by NVIDIA's RPC header, which says what the
 * message asks or reports and how many bytes the RPC header and the payload
 * take.
 *
 * The object keeps the driver's side of the command queue: its write pointer
 * and the next message's sequence number. It owns nothing, so the memory must
 * outlive it. Its calls never wait, and only one thread may use it at a time.
 */
class message_queues {
public:
    /**
     * @brief Lay the queue memory out as NVIDIA's driver does before the
     * firmware starts: zero the memory, write the page table and write the
     * command queue's header. The firmware writes the status queue's header.
     * @param memory The CPU's mapping of `QUEUE_MEMORY_BYTES` bytes. It must
     * be aligned to 8 bytes, since the page table is written in 8-byte
     * entries, and be normal memory rather than device memory, since messages
     * are copied in at any alignment, mapped so the card sees the CPU's writes
     * and the CPU sees the card's without cache maintenance.
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
     * driver reads it before checking the status queue's header. The command
     * queue's header asks that each reader keep its read pointer in the header
     * page of the queue it writes, so a firmware whose status queue's header
     * asks the same keeps its own in the status queue's header page, which
     * `send` never reads, and `send` then returns `ERR_QUEUE_FULL` once the
     * ring's free elements are used up. `send` touches none of the card's
     * registers, so after `OK` the caller must tell the firmware that a
     * message waits.
     * @param function NVIDIA's number for what the message asks of the
     * firmware.
     * @param payload The bytes the message carries after its headers, which
     * may be null when `payload_bytes` is 0.
     * @param payload_bytes How many bytes `payload` holds, refused above
     * `MESSAGE_PAYLOAD_MAX_BYTES`.
     * @return `OK`, `ERR_QUEUE_FULL` when the read pointer in the command
     * queue's header page lies past the ring or leaves too few elements free
     * for the message, or `ERR_TOO_LARGE`, which is logged, when
     * `payload_bytes` exceeds `MESSAGE_PAYLOAD_MAX_BYTES`. Either error writes
     * nothing. A caller may retry `ERR_QUEUE_FULL` once that read pointer
     * moves on.
     */
    int32_t send(uint32_t function, const void* payload, uint32_t payload_bytes);

private:
    uint8_t* m_command_queue;
    uint32_t m_write_pointer;
    uint32_t m_sequence_number;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_MESSAGE_QUEUES_H
