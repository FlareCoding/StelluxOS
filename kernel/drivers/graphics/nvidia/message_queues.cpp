// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "common/string.h"
#include "hw/barrier.h"
#include "sync/atomic.h"

namespace drivers::nvidia {

namespace {

// The header a queue's writer keeps at the start of the queue's header page
struct queue_header {
    uint32_t version;
    uint32_t bytes;               // of the whole queue, its header page included
    uint32_t element_bytes;
    uint32_t element_count;
    uint32_t write_pointer;
    uint32_t flags;
    uint32_t read_pointer_offset; // bytes from the queue's start to the read pointer its header page holds
    uint32_t elements_offset;     // bytes from the queue's start to its first element
};

// The element header and the RPC header every message starts with
struct message_headers {
    uint8_t  authentication[32]; // NVIDIA's authentication tag and additional authenticated data, 0 as nothing is
                                 // encrypted
    uint32_t checksum;
    uint32_t sequence_number;
    uint32_t element_count;
    uint32_t padding;            // 0, the gap NVIDIA's 8-byte alignment of the RPC header leaves
    uint32_t rpc_header_version;
    uint32_t rpc_signature;
    uint32_t rpc_bytes;          // of the RPC header and the payload
    uint32_t function;
    uint32_t result;
    uint32_t private_result;
    uint32_t rpc_sequence;
    uint32_t spare;
};

} // namespace

// The queue memory's layout: a page table in the first page, then the command queue and the status queue
constexpr uint32_t PAGE_BYTES           = 0x1000;
constexpr uint32_t PAGE_TABLE_ENTRIES   = QUEUE_MEMORY_BYTES / PAGE_BYTES;
constexpr uint32_t QUEUE_BYTES          = 0x40000;
constexpr uint32_t COMMAND_QUEUE_OFFSET = PAGE_BYTES;

static_assert(COMMAND_QUEUE_OFFSET + 2 * QUEUE_BYTES == QUEUE_MEMORY_BYTES, "the status queue ends the memory");

// The command queue's layout as NVIDIA's driver creates it: a header page, then 4 KiB elements to the queue's end
constexpr uint32_t QUEUE_VERSION       = 0;
constexpr uint32_t ELEMENT_BYTES       = 0x1000;
constexpr uint32_t READ_POINTER_OFFSET = 0x20;
constexpr uint32_t ELEMENTS_OFFSET     = 0x1000;
constexpr uint32_t ELEMENT_COUNT       = (QUEUE_BYTES - ELEMENTS_OFFSET) / ELEMENT_BYTES;

// The most elements one message fills
constexpr uint32_t MESSAGE_ELEMENTS_MAX = 16;

// Asks that each reader keep its read pointer in the header page of the queue it writes, which takes effect only when
// both queues' headers ask
constexpr uint32_t SWAPS_READ_POINTERS = 0x1;

// What NVIDIA's driver writes in the RPC header of every message it sends
constexpr uint32_t RPC_HEADER_VERSION = 0x03000000;
constexpr uint32_t RPC_SIGNATURE      = 0x43505256;
constexpr uint32_t RPC_RESULT_PENDING = 0xFFFFFFFF;

// The checksum covers the message in 8-byte words, the last one padded with zeros
constexpr uint32_t CHECKSUM_WORD_BYTES = 8;

constexpr uint32_t ELEMENT_HEADER_BYTES  = 48;
constexpr uint32_t RPC_HEADER_BYTES      = 32;
constexpr uint32_t MESSAGE_HEADERS_BYTES = sizeof(message_headers);

static_assert(
    MESSAGE_HEADERS_BYTES == ELEMENT_HEADER_BYTES + RPC_HEADER_BYTES,
    "the RPC header follows the element header"
);
static_assert(
    MESSAGE_PAYLOAD_MAX_BYTES + MESSAGE_HEADERS_BYTES == MESSAGE_ELEMENTS_MAX * ELEMENT_BYTES,
    "the largest payload fills 16 elements"
);
static_assert(MESSAGE_HEADERS_BYTES % CHECKSUM_WORD_BYTES == 0, "the payload starts on a checksum word");

// The XOR of the 8-byte little-endian words in the `bytes` bytes at `data`, the last word padded with zeros
static uint64_t xor_words(const uint8_t* data, uint32_t bytes) {
    uint64_t sum = 0;
    uint32_t whole_bytes = bytes - bytes % CHECKSUM_WORD_BYTES;
    for (uint32_t i = 0; i < whole_bytes; i += CHECKSUM_WORD_BYTES) {
        uint64_t word = 0;
        string::memcpy(&word, data + i, CHECKSUM_WORD_BYTES);
        sum ^= word;
    }

    uint64_t last_word = 0;
    string::memcpy(&last_word, data + whole_bytes, bytes - whole_bytes);

    return sum ^ last_word;
}

static uint8_t* element(uint8_t* queue, uint32_t index) {
    return queue + ELEMENTS_OFFSET + index * ELEMENT_BYTES;
}

message_queues::message_queues(uint8_t* memory, uint64_t dma_address)
    : m_command_queue(memory + COMMAND_QUEUE_OFFSET)
    , m_write_pointer(0)
    , m_sequence_number(0) {
    string::memset(memory, 0, QUEUE_MEMORY_BYTES);

    uint64_t* page_table = reinterpret_cast<uint64_t*>(memory);
    for (uint32_t i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        page_table[i] = dma_address + static_cast<uint64_t>(i) * PAGE_BYTES;
    }

    queue_header header = {};
    header.version = QUEUE_VERSION;
    header.bytes = QUEUE_BYTES;
    header.element_bytes = ELEMENT_BYTES;
    header.element_count = ELEMENT_COUNT;
    header.flags = SWAPS_READ_POINTERS;
    header.read_pointer_offset = READ_POINTER_OFFSET;
    header.elements_offset = ELEMENTS_OFFSET;

    string::memcpy(m_command_queue, &header, sizeof(header));
}

int32_t message_queues::send(uint32_t function, const void* payload, uint32_t payload_bytes) {
    if (payload_bytes > MESSAGE_PAYLOAD_MAX_BYTES) {
        log::error(
            "nvidia: a message for function %u was not sent to the GSP's firmware, as its %u-byte payload exceeds "
            "the %u bytes one message carries",
            function,
            payload_bytes,
            MESSAGE_PAYLOAD_MAX_BYTES
        );
        return ERR_TOO_LARGE;
    }

    uint32_t message_bytes = MESSAGE_HEADERS_BYTES + payload_bytes;
    uint32_t element_count = (message_bytes + ELEMENT_BYTES - 1) / ELEMENT_BYTES;
    uint32_t next_write_pointer = (m_write_pointer + element_count) % ELEMENT_COUNT;

    // A read pointer past the ring leaves no room, as NVIDIA's count of free elements treats it
    uint32_t* firmware_read_pointer = reinterpret_cast<uint32_t*>(m_command_queue + READ_POINTER_OFFSET);
    uint32_t read_pointer = sync::atomic_ref<uint32_t>{*firmware_read_pointer}.load_acquire();
    bool is_read_pointer_in_ring = read_pointer < ELEMENT_COUNT;
    uint32_t free_elements =
        is_read_pointer_in_ring ? (read_pointer + ELEMENT_COUNT - m_write_pointer - 1) % ELEMENT_COUNT : 0;
    if (element_count > free_elements) {
        return ERR_QUEUE_FULL;
    }

    const uint8_t* payload_data = static_cast<const uint8_t*>(payload);
    message_headers headers = {};
    headers.sequence_number = m_sequence_number;
    headers.element_count = element_count;
    headers.rpc_header_version = RPC_HEADER_VERSION;
    headers.rpc_signature = RPC_SIGNATURE;
    headers.rpc_bytes = message_bytes - ELEMENT_HEADER_BYTES;
    headers.function = function;
    headers.result = RPC_RESULT_PENDING;
    headers.private_result = RPC_RESULT_PENDING;

    // With the checksum field included, the XOR of the message's words folded to 32 bits by XORing its halves is 0
    const uint8_t* headers_data = reinterpret_cast<const uint8_t*>(&headers);
    uint64_t sum = xor_words(headers_data, MESSAGE_HEADERS_BYTES) ^ xor_words(payload_data, payload_bytes);
    headers.checksum = static_cast<uint32_t>(sum >> 32) ^ static_cast<uint32_t>(sum);

    uint32_t index = m_write_pointer;
    string::memcpy(element(m_command_queue, index), &headers, MESSAGE_HEADERS_BYTES);

    uint32_t offset = MESSAGE_HEADERS_BYTES;
    uint32_t copied = 0;
    while (copied < payload_bytes) {
        if (offset == ELEMENT_BYTES) {
            index = (index + 1) % ELEMENT_COUNT;
            offset = 0;
        }

        uint32_t chunk = ELEMENT_BYTES - offset;
        if (chunk > payload_bytes - copied) {
            chunk = payload_bytes - copied;
        }

        string::memcpy(element(m_command_queue, index) + offset, payload_data + copied, chunk);
        copied += chunk;
        offset += chunk;
    }

    // Zeros fill the rest of the last element, as NVIDIA's driver leaves it, and pad the checksum's last word
    string::memset(element(m_command_queue, index) + offset, 0, ELEMENT_BYTES - offset);

    // The card must see the whole message before the write pointer that hands it over
    queue_header* command_queue = reinterpret_cast<queue_header*>(m_command_queue);
    barrier::dma_write();
    sync::atomic_ref<uint32_t>{command_queue->write_pointer}.store_relaxed(next_write_pointer);

    m_write_pointer = next_write_pointer;
    m_sequence_number++;

    return OK;
}

} // namespace drivers::nvidia
