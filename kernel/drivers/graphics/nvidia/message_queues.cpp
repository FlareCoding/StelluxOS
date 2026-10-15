// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "common/string.h"
#include "hw/barrier.h"
#include "sync/atomic.h"

namespace drivers::nvidia {

namespace {

// The header a queue's writer keeps at the queue's start
struct queue_header {
    uint32_t version;
    uint32_t bytes;               // of the whole queue, its header included
    uint32_t element_bytes;
    uint32_t element_count;
    uint32_t write_pointer;
    uint32_t flags;
    uint32_t read_pointer_offset; // bytes from the queue's start to the read pointer kept before its first element
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
constexpr uint32_t STATUS_QUEUE_OFFSET  = COMMAND_QUEUE_OFFSET + QUEUE_BYTES;

static_assert(STATUS_QUEUE_OFFSET + QUEUE_BYTES == QUEUE_MEMORY_BYTES, "the status queue ends the memory");

// The command queue's layout as NVIDIA's driver creates it: a header page, then 4 KiB elements to the queue's end.
// The status queue's header must give the same version, queue size and element size
constexpr uint32_t QUEUE_VERSION       = 0;
constexpr uint32_t ELEMENT_BYTES       = 0x1000;
constexpr uint32_t READ_POINTER_OFFSET = 0x20;
constexpr uint32_t ELEMENTS_OFFSET     = 0x1000;
constexpr uint32_t ELEMENT_COUNT       = (QUEUE_BYTES - ELEMENTS_OFFSET) / ELEMENT_BYTES;

// The most elements one message fills
constexpr uint32_t MESSAGE_ELEMENTS_MAX = 16;

// Asks that each reader keep its read pointer in the queue it writes, taking effect only when both queues' headers ask
constexpr uint32_t SWAPS_READ_POINTERS = 0x1;

// What NVIDIA's driver writes in the RPC header of every message it sends
constexpr uint32_t RPC_HEADER_VERSION = 0x03000000;
constexpr uint32_t RPC_SIGNATURE      = 0x43505256;
constexpr uint32_t RPC_RESULT_PENDING = 0xFFFFFFFF;

// The checksum covers the message in 8-byte words, the last one filled out by the bytes that follow the message
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

// NVIDIA's checksum folds the XOR of a message's words to 32 bits, and a message passes when the fold is 0
static uint32_t fold_checksum(uint64_t sum) {
    return static_cast<uint32_t>(sum >> 32) ^ static_cast<uint32_t>(sum);
}

static uint8_t* element(uint8_t* queue, uint32_t index) {
    return queue + ELEMENTS_OFFSET + index * ELEMENT_BYTES;
}

message_queues::message_queues(uint8_t* memory, uint64_t dma_address)
    : m_command_queue(memory + COMMAND_QUEUE_OFFSET)
    , m_firmware_read_pointer(reinterpret_cast<uint32_t*>(m_command_queue + READ_POINTER_OFFSET))
    , m_write_pointer(0)
    , m_sequence_number(0)
    , m_status_queue(memory + STATUS_QUEUE_OFFSET)
    , m_is_status_queue_linked(false)
    , m_status_elements(nullptr)
    , m_status_element_count(0)
    , m_driver_read_pointer(nullptr)
    , m_status_read_pointer(0)
    , m_status_sequence_number(0) {
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
            "nvidia: a message for function 0x%X was not sent to the GSP's firmware, as its %u-byte payload exceeds "
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
    uint32_t read_pointer = sync::atomic_ref<uint32_t>{*m_firmware_read_pointer}.load_acquire();
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

    // With the checksum field included, the XOR of the message's words folds to 0
    const uint8_t* headers_data = reinterpret_cast<const uint8_t*>(&headers);
    uint64_t sum = xor_words(headers_data, MESSAGE_HEADERS_BYTES) ^ xor_words(payload_data, payload_bytes);
    headers.checksum = fold_checksum(sum);

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

int32_t message_queues::link_status_queue() {
    if (m_is_status_queue_linked) {
        return OK;
    }

    // NVIDIA's driver fences fully before each link, so the header is read after all the caller did before
    barrier::dma_full();
    queue_header header = {};
    string::memcpy(&header, m_status_queue, sizeof(header));

    // NVIDIA's checks, without 32-bit wraps, and the driver's own rule of an aligned read pointer before the elements,
    // which keeps its atomic accesses off them and implies NVIDIA's rule that the elements start 0x24 bytes in or later
    bool has_nvidias_version_and_sizes = header.version == QUEUE_VERSION
        && header.bytes == QUEUE_BYTES
        && header.element_bytes == ELEMENT_BYTES;
    bool has_elements = lies_inside(header.elements_offset, ELEMENT_BYTES, QUEUE_BYTES)
        && header.element_count == (QUEUE_BYTES - header.elements_offset) / ELEMENT_BYTES;
    bool is_read_pointer_aligned_before_elements = header.read_pointer_offset >= sizeof(queue_header)
        && header.read_pointer_offset % sizeof(uint32_t) == 0
        && lies_inside(header.read_pointer_offset, sizeof(uint32_t), header.elements_offset);
    if (!has_nvidias_version_and_sizes || !has_elements || !is_read_pointer_aligned_before_elements) {
        return ERR_NOT_LINKED;
    }

    // The command queue's header always asks to swap, so the firmware's flag decides
    bool swaps_read_pointers = (header.flags & SWAPS_READ_POINTERS) != 0;
    uint32_t* command_queue_read_pointer = reinterpret_cast<uint32_t*>(m_command_queue + READ_POINTER_OFFSET);
    uint32_t* status_queue_read_pointer = reinterpret_cast<uint32_t*>(m_status_queue + header.read_pointer_offset);

    m_firmware_read_pointer = swaps_read_pointers ? status_queue_read_pointer : command_queue_read_pointer;
    m_driver_read_pointer = swaps_read_pointers ? command_queue_read_pointer : status_queue_read_pointer;
    m_status_elements = m_status_queue + header.elements_offset;
    m_status_element_count = header.element_count;
    m_is_status_queue_linked = true;

    sync::atomic_ref<uint32_t>{*m_driver_read_pointer}.store_relaxed(0);

    return OK;
}

int32_t message_queues::receive(received_message& out, void* payload, uint32_t payload_capacity) {
    if (!m_is_status_queue_linked) {
        return ERR_NOT_LINKED;
    }

    // NVIDIA's driver fences fully before each receive, so the write pointer is read after all the caller did before
    barrier::dma_full();

    // A write pointer past the ring leaves nothing to read, as NVIDIA's count of written elements treats it once its
    // cached count is spent, where the driver reads the write pointer on every call
    queue_header* status_queue = reinterpret_cast<queue_header*>(m_status_queue);
    uint32_t write_pointer = sync::atomic_ref<uint32_t>{status_queue->write_pointer}.load_relaxed();
    bool is_write_pointer_in_ring = write_pointer < m_status_element_count;
    uint32_t elements_before_write_pointer =
        (write_pointer + m_status_element_count - m_status_read_pointer) % m_status_element_count;
    uint32_t written_elements = is_write_pointer_in_ring ? elements_before_write_pointer : 0;
    if (written_elements == 0) {
        return ERR_QUEUE_EMPTY;
    }

    // The firmware writes a message before the write pointer that hands it over, so the message is read after it
    barrier::dma_read();
    message_headers headers = {};
    string::memcpy(&headers, m_status_elements + m_status_read_pointer * ELEMENT_BYTES, MESSAGE_HEADERS_BYTES);

    // NVIDIA's driver fails a message whose elements the write pointer has not all passed. The 1 to 16 bound is the
    // driver's own, so `rpc_bytes_max` cannot wrap and every payload fits `MESSAGE_PAYLOAD_MAX_BYTES`
    uint32_t element_count = headers.element_count;
    bool is_element_count_in_range = element_count >= 1 && element_count <= MESSAGE_ELEMENTS_MAX;
    bool are_elements_written = element_count <= written_elements;
    if (!is_element_count_in_range || !are_elements_written) {
        log::error(
            "nvidia: the message from the GSP's firmware at status queue element %u was refused, as its element "
            "header gives %u elements, where a message fills 1 to %u and the write pointer "
            "lies %u elements past its first",
            m_status_read_pointer,
            element_count,
            MESSAGE_ELEMENTS_MAX,
            written_elements
        );
        return ERR_BAD_MESSAGE;
    }

    uint32_t rpc_bytes = headers.rpc_bytes;
    uint32_t rpc_bytes_max = element_count * ELEMENT_BYTES - ELEMENT_HEADER_BYTES;
    bool is_rpc_length_valid = rpc_bytes >= RPC_HEADER_BYTES && rpc_bytes <= rpc_bytes_max;
    if (!is_rpc_length_valid) {
        log::error(
            "nvidia: the message from the GSP's firmware at status queue element %u was refused, as its RPC header "
            "gives %u bytes for itself and the payload where %u to %u fit an element count of %u",
            m_status_read_pointer,
            rpc_bytes,
            RPC_HEADER_BYTES,
            rpc_bytes_max,
            element_count
        );
        return ERR_BAD_MESSAGE;
    }

    // NVIDIA's driver sums whole words, so the bytes padding the last word count as the firmware left them. The
    // headers' words come from the copy whose fields are used
    uint32_t message_bytes = ELEMENT_HEADER_BYTES + rpc_bytes;
    uint32_t checked_bytes = (message_bytes + CHECKSUM_WORD_BYTES - 1) / CHECKSUM_WORD_BYTES * CHECKSUM_WORD_BYTES;
    uint64_t sum = xor_words(reinterpret_cast<const uint8_t*>(&headers), MESSAGE_HEADERS_BYTES);
    uint32_t checked = MESSAGE_HEADERS_BYTES;
    while (checked < checked_bytes) {
        uint32_t index = (m_status_read_pointer + checked / ELEMENT_BYTES) % m_status_element_count;
        uint32_t offset = checked % ELEMENT_BYTES;
        uint32_t bytes = ELEMENT_BYTES - offset;
        if (bytes > checked_bytes - checked) {
            bytes = checked_bytes - checked;
        }

        sum ^= xor_words(m_status_elements + index * ELEMENT_BYTES + offset, bytes);
        checked += bytes;
    }

    uint32_t checksum = fold_checksum(sum);
    if (checksum != 0) {
        log::error(
            "nvidia: the message from the GSP's firmware at status queue element %u was refused, as its checksum "
            "fails, the XOR of its 8-byte words folding to 0x%08X rather than 0",
            m_status_read_pointer,
            checksum
        );
        return ERR_BAD_MESSAGE;
    }

    if (headers.sequence_number != m_status_sequence_number) {
        log::error(
            "nvidia: the message from the GSP's firmware at status queue element %u was refused, as its sequence "
            "number is %u where %u is due, counting the status queue's messages from 0",
            m_status_read_pointer,
            headers.sequence_number,
            m_status_sequence_number
        );
        return ERR_BAD_MESSAGE;
    }

    uint32_t payload_bytes = rpc_bytes - RPC_HEADER_BYTES;
    if (payload_bytes > payload_capacity) {
        log::error(
            "nvidia: a message for function 0x%X was not received from the GSP's firmware, as its %u-byte payload "
            "exceeds the %u bytes given for it",
            headers.function,
            payload_bytes,
            payload_capacity
        );
        return ERR_TOO_LARGE;
    }

    uint8_t* payload_data = static_cast<uint8_t*>(payload);
    uint32_t copied = 0;
    while (copied < payload_bytes) {
        uint32_t message_offset = MESSAGE_HEADERS_BYTES + copied;
        uint32_t index = (m_status_read_pointer + message_offset / ELEMENT_BYTES) % m_status_element_count;
        uint32_t offset = message_offset % ELEMENT_BYTES;
        uint32_t chunk = ELEMENT_BYTES - offset;
        if (chunk > payload_bytes - copied) {
            chunk = payload_bytes - copied;
        }

        string::memcpy(payload_data + copied, m_status_elements + index * ELEMENT_BYTES + offset, chunk);
        copied += chunk;
    }

    out.function = headers.function;
    out.result = headers.result;
    out.payload_bytes = payload_bytes;

    // The firmware may rewrite the elements once the read pointer passes them, so every read of them comes first
    uint32_t next_read_pointer = (m_status_read_pointer + element_count) % m_status_element_count;
    barrier::dma_full();
    sync::atomic_ref<uint32_t>{*m_driver_read_pointer}.store_relaxed(next_read_pointer);

    m_status_read_pointer = next_read_pointer;
    m_status_sequence_number++;

    return OK;
}

} // namespace drivers::nvidia
