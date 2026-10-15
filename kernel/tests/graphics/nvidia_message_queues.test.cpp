// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_message_queues);

using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_NOT_LINKED;
using drivers::nvidia::ERR_QUEUE_EMPTY;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TOO_LARGE;
using drivers::nvidia::MESSAGE_PAYLOAD_MAX_BYTES;
using drivers::nvidia::OK;
using drivers::nvidia::QUEUE_MEMORY_BYTES;
using drivers::nvidia::message_queues;
using drivers::nvidia::received_message;

// The queue memory's layout and a queue's, as NVIDIA's driver lays them out, restated here so the tests hold the
// queues to NVIDIA's layout rather than to their own constants
static constexpr uint32_t MEMORY_BYTES         = 0x81000;
static constexpr uint32_t PAGE_BYTES           = 0x1000;
static constexpr uint32_t PAGE_TABLE_ENTRIES   = 129;
static constexpr uint32_t COMMAND_QUEUE_OFFSET = 0x1000;
static constexpr uint32_t STATUS_QUEUE_OFFSET  = 0x41000;
static constexpr uint32_t QUEUE_BYTES          = 0x40000;
static constexpr uint32_t ELEMENTS_OFFSET      = 0x1000;
static constexpr uint32_t ELEMENT_BYTES        = 0x1000;
static constexpr uint32_t ELEMENT_COUNT        = 63;

// The most elements one message fills
static constexpr uint32_t MESSAGE_ELEMENTS = 16;

// A queue header's fields and the read pointer NVIDIA's layout puts after them, as offsets from the queue's start
static constexpr uint32_t HEADER_VERSION             = 0x00;
static constexpr uint32_t HEADER_BYTES               = 0x04;
static constexpr uint32_t HEADER_ELEMENT_BYTES       = 0x08;
static constexpr uint32_t HEADER_ELEMENT_COUNT       = 0x0C;
static constexpr uint32_t HEADER_WRITE_POINTER       = 0x10;
static constexpr uint32_t HEADER_FLAGS               = 0x14;
static constexpr uint32_t HEADER_READ_POINTER_OFFSET = 0x18;
static constexpr uint32_t HEADER_ELEMENTS_OFFSET     = 0x1C;
static constexpr uint32_t READ_POINTER               = 0x20;

static constexpr uint32_t SWAPS_READ_POINTERS = 0x1;

// A message's fields, as offsets from its first byte
static constexpr uint32_t MESSAGE_CHECKSUM           = 32;
static constexpr uint32_t MESSAGE_SEQUENCE_NUMBER    = 36;
static constexpr uint32_t MESSAGE_ELEMENT_COUNT      = 40;
static constexpr uint32_t MESSAGE_PADDING            = 44;
static constexpr uint32_t MESSAGE_RPC_HEADER_VERSION = 48;
static constexpr uint32_t MESSAGE_RPC_SIGNATURE      = 52;
static constexpr uint32_t MESSAGE_RPC_BYTES          = 56;
static constexpr uint32_t MESSAGE_FUNCTION           = 60;
static constexpr uint32_t MESSAGE_RESULT             = 64;
static constexpr uint32_t MESSAGE_PRIVATE_RESULT     = 68;
static constexpr uint32_t MESSAGE_RPC_SEQUENCE       = 72;
static constexpr uint32_t MESSAGE_SPARE              = 76;
static constexpr uint32_t MESSAGE_PAYLOAD            = 80;

static constexpr uint32_t RPC_HEADER_BYTES = 32;

// The most payload bytes a message filling one element carries, and a message filling all 16
static constexpr uint32_t FIRST_ELEMENT_PAYLOAD_BYTES = ELEMENT_BYTES - MESSAGE_PAYLOAD;
static constexpr uint32_t PAYLOAD_MAX_BYTES           = MESSAGE_ELEMENTS * ELEMENT_BYTES - MESSAGE_PAYLOAD;

// The checksum covers the message in 8-byte words
static constexpr uint32_t CHECKSUM_WORD_BYTES = 8;

// What NVIDIA's driver writes in the RPC header of every message it sends
static constexpr uint32_t RPC_HEADER_VERSION = 0x03000000;
static constexpr uint32_t RPC_SIGNATURE      = 0x43505256;
static constexpr uint32_t RPC_RESULT_PENDING = 0xFFFFFFFF;

// An address above 4 GiB, so a page table entry cut to 32 bits shows
static constexpr uint64_t DMA_ADDRESS = 0x123456789000;

// A function number above 16 bits, so a function number cut to 16 bits shows
static constexpr uint32_t FUNCTION = 0x12345;

// A result above 16 bits, so a result cut to 16 bits shows
static constexpr uint32_t RESULT = 0x87654321;

// What the fake memory holds wherever the queues have written nothing
static constexpr uint8_t STALE_BYTE = 0xA5;

// What a received message's fields and a read pointer hold where the driver has written nothing
static constexpr uint32_t STALE_WORD = 0xA5A5A5A5;

// Bytes of a receive buffer that must stay stale: those past the room `expect_received` gives, and all of the buffer
// `expect_refused` gives
static constexpr uint32_t SPARE_BUFFER_BYTES = 64;

// A fake of the queue memory with one element of guard after it, all filled with `STALE_BYTE`, so a message read
// past the status queue's end shows
struct fake_queue_memory {
    uint8_t* bytes = static_cast<uint8_t*>(heap::ualloc(MEMORY_BYTES + ELEMENT_BYTES));

    fake_queue_memory() {
        if (bytes != nullptr) {
            string::memset(bytes, STALE_BYTE, MEMORY_BYTES + ELEMENT_BYTES);
        }
    }

    ~fake_queue_memory() { heap::ufree(bytes); }

    uint32_t read(uint32_t offset) const { return *reinterpret_cast<uint32_t*>(bytes + offset); }
    void write(uint32_t offset, uint32_t value) { *reinterpret_cast<uint32_t*>(bytes + offset) = value; }

    uint8_t* element(uint32_t index) const {
        return bytes + COMMAND_QUEUE_OFFSET + ELEMENTS_OFFSET + index * ELEMENT_BYTES;
    }
};

// A status queue header a test writes in the firmware's place, by default laid out as the command queue is
struct status_queue_test_header {
    uint32_t version             = 0;
    uint32_t bytes               = QUEUE_BYTES;
    uint32_t element_bytes       = ELEMENT_BYTES;
    uint32_t element_count       = ELEMENT_COUNT;
    uint32_t write_pointer       = 0;
    uint32_t flags               = SWAPS_READ_POINTERS;
    uint32_t read_pointer_offset = READ_POINTER;
    uint32_t elements_offset     = ELEMENTS_OFFSET;

    void write_to(fake_queue_memory& memory) const {
        string::memcpy(memory.bytes + STATUS_QUEUE_OFFSET, this, sizeof(*this));
    }

    // Where the driver keeps its read pointer of the status queue once linked, and where it then reads the firmware's
    uint32_t driver_read_pointer() const {
        return swaps() ? COMMAND_QUEUE_OFFSET + READ_POINTER : STATUS_QUEUE_OFFSET + read_pointer_offset;
    }

    uint32_t firmware_read_pointer() const {
        return swaps() ? STATUS_QUEUE_OFFSET + read_pointer_offset : COMMAND_QUEUE_OFFSET + READ_POINTER;
    }

    bool swaps() const { return (flags & SWAPS_READ_POINTERS) != 0; }
};

// A status queue whose read pointer and elements lie elsewhere than the command queue's, with one element fewer
static constexpr uint32_t MOVED_READ_POINTER_OFFSET = 0x40;
static constexpr uint32_t MOVED_ELEMENTS_OFFSET     = 0x2000;
static constexpr status_queue_test_header MOVED_HEADER = {
    .element_count       = (QUEUE_BYTES - MOVED_ELEMENTS_OFFSET) / ELEMENT_BYTES,
    .read_pointer_offset = MOVED_READ_POINTER_OFFSET,
    .elements_offset     = MOVED_ELEMENTS_OFFSET,
};

// A message framed as NVIDIA's driver reads the firmware's from the status queue, in room for `element_capacity`
// elements, for a test to change, seal again and store
struct status_test_message {
    uint32_t elements = 0;
    uint8_t* bytes;

    explicit status_test_message(uint32_t element_capacity)
        : bytes(static_cast<uint8_t*>(heap::ualloc(element_capacity * ELEMENT_BYTES))) {}

    ~status_test_message() { heap::ufree(bytes); }

    // Frames a message of `FUNCTION` and `RESULT` filling `element_count` elements, zeros following the payload
    void frame(uint32_t element_count, uint32_t sequence_number, const uint8_t* payload, uint32_t payload_bytes);

    void set(uint32_t offset, uint32_t value) { *reinterpret_cast<uint32_t*>(bytes + offset) = value; }

    // Writes the checksum that folds the first `message_bytes` bytes, in whole 8-byte words, to 0
    void seal(uint32_t message_bytes);

    // Copies the framed elements into the status queue the header lays out, from element `first` on
    void store(fake_queue_memory& memory, const status_queue_test_header& header, uint32_t first) const {
        for (uint32_t i = 0; i < elements; i++) {
            uint32_t index = (first + i) % header.element_count;
            uint32_t offset = STATUS_QUEUE_OFFSET + header.elements_offset + index * ELEMENT_BYTES;
            string::memcpy(memory.bytes + offset, bytes + i * ELEMENT_BYTES, ELEMENT_BYTES);
        }
    }
};

// A payload of `count` bytes in a pattern that repeats every 253 bytes and holds no zero, so a payload copied to the
// wrong offset or not at all shows
struct queue_test_payload {
    uint32_t count;
    uint8_t* bytes;

    explicit queue_test_payload(uint32_t payload_bytes)
        : count(payload_bytes)
        , bytes(static_cast<uint8_t*>(heap::ualloc(payload_bytes))) {
        for (uint32_t i = 0; bytes != nullptr && i < count; i++) {
            bytes[i] = static_cast<uint8_t>(i % 253 + 1);
        }
    }

    ~queue_test_payload() { heap::ufree(bytes); }
};

// NVIDIA's checksum of a message's first `bytes` bytes, which must be a multiple of 8: the XOR of its 8-byte words,
// folded to 32 bits
static uint32_t nvidia_checksum(const uint8_t* message, uint32_t bytes) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < bytes; i += CHECKSUM_WORD_BYTES) {
        uint64_t word = 0;
        string::memcpy(&word, message + i, sizeof(word));
        sum ^= word;
    }

    return static_cast<uint32_t>(sum >> 32) ^ static_cast<uint32_t>(sum);
}

// The bytes NVIDIA's checksum covers of a message of `message_bytes` bytes, rounded up to whole words
static constexpr uint32_t checked_bytes_for(uint32_t message_bytes) {
    return (message_bytes + CHECKSUM_WORD_BYTES - 1) / CHECKSUM_WORD_BYTES * CHECKSUM_WORD_BYTES;
}

void status_test_message::frame(
    uint32_t element_count,
    uint32_t sequence_number,
    const uint8_t* payload,
    uint32_t payload_bytes
) {
    elements = element_count;
    string::memset(bytes, 0, element_count * ELEMENT_BYTES);

    set(MESSAGE_SEQUENCE_NUMBER, sequence_number);
    set(MESSAGE_ELEMENT_COUNT, element_count);
    set(MESSAGE_RPC_HEADER_VERSION, RPC_HEADER_VERSION);
    set(MESSAGE_RPC_SIGNATURE, RPC_SIGNATURE);
    set(MESSAGE_RPC_BYTES, RPC_HEADER_BYTES + payload_bytes);
    set(MESSAGE_FUNCTION, FUNCTION);
    set(MESSAGE_RESULT, RESULT);
    string::memcpy(bytes + MESSAGE_PAYLOAD, payload, payload_bytes);

    seal(MESSAGE_PAYLOAD + payload_bytes);
}

void status_test_message::seal(uint32_t message_bytes) {
    set(MESSAGE_CHECKSUM, 0);
    set(MESSAGE_CHECKSUM, nvidia_checksum(bytes, checked_bytes_for(message_bytes)));
}

static uint32_t elements_for(uint32_t payload_bytes) {
    return (MESSAGE_PAYLOAD + payload_bytes + ELEMENT_BYTES - 1) / ELEMENT_BYTES;
}

static uint32_t field_at(const uint8_t* message, uint32_t offset) {
    return *reinterpret_cast<const uint32_t*>(message + offset);
}

static bool is_stale(const uint8_t* bytes, uint32_t count) {
    uint32_t changed_bytes = 0;
    for (uint32_t i = 0; i < count; i++) {
        changed_bytes += bytes[i] != STALE_BYTE ? 1 : 0;
    }

    return changed_bytes == 0;
}

// Checks the message in the command queue's elements from `first` on against what NVIDIA's driver writes for a
// message of `FUNCTION` with this sequence number and payload
static void expect_message(
    const fake_queue_memory& memory,
    uint32_t first,
    uint32_t sequence_number,
    const uint8_t* payload,
    uint32_t payload_bytes
) {
    uint32_t element_count = elements_for(payload_bytes);
    uint8_t* message = static_cast<uint8_t*>(heap::ualloc(element_count * ELEMENT_BYTES));
    ASSERT_NOT_NULL(message);

    for (uint32_t i = 0; i < element_count; i++) {
        string::memcpy(message + i * ELEMENT_BYTES, memory.element((first + i) % ELEMENT_COUNT), ELEMENT_BYTES);
    }

    uint32_t nonzero_authentication_bytes = 0;
    for (uint32_t i = 0; i < MESSAGE_CHECKSUM; i++) {
        nonzero_authentication_bytes += message[i] != 0 ? 1 : 0;
    }

    EXPECT_EQ(nonzero_authentication_bytes, 0u);
    EXPECT_EQ(field_at(message, MESSAGE_SEQUENCE_NUMBER), sequence_number);
    EXPECT_EQ(field_at(message, MESSAGE_ELEMENT_COUNT), element_count);
    EXPECT_EQ(field_at(message, MESSAGE_PADDING), 0u);
    EXPECT_EQ(field_at(message, MESSAGE_RPC_HEADER_VERSION), RPC_HEADER_VERSION);
    EXPECT_EQ(field_at(message, MESSAGE_RPC_SIGNATURE), RPC_SIGNATURE);
    EXPECT_EQ(field_at(message, MESSAGE_RPC_BYTES), RPC_HEADER_BYTES + payload_bytes);
    EXPECT_EQ(field_at(message, MESSAGE_FUNCTION), FUNCTION);
    EXPECT_EQ(field_at(message, MESSAGE_RESULT), RPC_RESULT_PENDING);
    EXPECT_EQ(field_at(message, MESSAGE_PRIVATE_RESULT), RPC_RESULT_PENDING);
    EXPECT_EQ(field_at(message, MESSAGE_RPC_SEQUENCE), 0u);
    EXPECT_EQ(field_at(message, MESSAGE_SPARE), 0u);
    EXPECT_EQ(string::memcmp(message + MESSAGE_PAYLOAD, payload, payload_bytes), 0);

    uint32_t message_bytes = MESSAGE_PAYLOAD + payload_bytes;
    uint32_t nonzero_tail_bytes = 0;
    for (uint32_t i = message_bytes; i < element_count * ELEMENT_BYTES; i++) {
        nonzero_tail_bytes += message[i] != 0 ? 1 : 0;
    }

    EXPECT_EQ(nonzero_tail_bytes, 0u);
    EXPECT_EQ(nvidia_checksum(message, checked_bytes_for(message_bytes)), 0u);

    heap::ufree(message);
}

// Receives the next message into room for `payload_capacity` bytes, any payload by default, and checks it carries
// `FUNCTION`, `RESULT` and this payload, with nothing written past the payload
static void expect_received(
    message_queues& queues,
    const uint8_t* payload,
    uint32_t payload_bytes,
    uint32_t payload_capacity = MESSAGE_PAYLOAD_MAX_BYTES
) {
    uint32_t buffer_bytes = payload_capacity + SPARE_BUFFER_BYTES;
    uint8_t* buffer = static_cast<uint8_t*>(heap::ualloc(buffer_bytes));
    ASSERT_NOT_NULL(buffer);
    string::memset(buffer, STALE_BYTE, buffer_bytes);

    received_message out = {STALE_WORD, STALE_WORD, STALE_WORD};
    EXPECT_EQ(queues.receive(out, buffer, payload_capacity), OK);
    EXPECT_EQ(out.function, FUNCTION);
    EXPECT_EQ(out.result, RESULT);
    EXPECT_EQ(out.payload_bytes, payload_bytes);
    EXPECT_EQ(string::memcmp(buffer, payload, payload_bytes), 0);
    EXPECT_TRUE(is_stale(buffer + payload_bytes, buffer_bytes - payload_bytes));

    heap::ufree(buffer);
}

// Checks that two receives in a row return `error`, leaving the caller's message and buffer and the driver's read
// pointer unchanged
static void expect_refused(
    message_queues& queues,
    const fake_queue_memory& memory,
    const status_queue_test_header& header,
    int32_t error,
    uint32_t payload_capacity = SPARE_BUFFER_BYTES
) {
    uint8_t buffer[SPARE_BUFFER_BYTES];
    string::memset(buffer, STALE_BYTE, sizeof(buffer));
    uint32_t read_pointer = memory.read(header.driver_read_pointer());

    for (uint32_t attempt = 0; attempt < 2; attempt++) {
        received_message out = {STALE_WORD, STALE_WORD, STALE_WORD};
        EXPECT_EQ(queues.receive(out, buffer, payload_capacity), error);
        EXPECT_EQ(out.function, STALE_WORD);
        EXPECT_EQ(out.result, STALE_WORD);
        EXPECT_EQ(out.payload_bytes, STALE_WORD);
    }

    EXPECT_TRUE(is_stale(buffer, sizeof(buffer)));
    EXPECT_EQ(memory.read(header.driver_read_pointer()), read_pointer);
}

// The memory is zeroed but for the page table, which lists every page's address, and the command queue's header,
// which holds what NVIDIA's driver writes there
TEST(nvidia_message_queues, memory_is_laid_out_as_nvidia_lays_it_out) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);

    message_queues queues(memory.bytes, DMA_ADDRESS);

    EXPECT_EQ(QUEUE_MEMORY_BYTES, MEMORY_BYTES);
    uint32_t wrong_page_table_entries = 0;
    for (uint32_t i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        uint64_t entry = *reinterpret_cast<uint64_t*>(memory.bytes + i * sizeof(uint64_t));
        wrong_page_table_entries += entry != DMA_ADDRESS + static_cast<uint64_t>(i) * PAGE_BYTES ? 1 : 0;
    }

    EXPECT_EQ(wrong_page_table_entries, 0u);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_VERSION), 0u);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_BYTES), QUEUE_BYTES);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_ELEMENT_BYTES), ELEMENT_BYTES);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_ELEMENT_COUNT), ELEMENT_COUNT);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 0u);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_FLAGS), SWAPS_READ_POINTERS);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_READ_POINTER_OFFSET), READ_POINTER);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_ELEMENTS_OFFSET), ELEMENTS_OFFSET);

    uint32_t nonzero_bytes = 0;
    for (uint32_t offset = PAGE_TABLE_ENTRIES * sizeof(uint64_t); offset < MEMORY_BYTES; offset++) {
        bool is_command_queue_header = offset >= COMMAND_QUEUE_OFFSET && offset < COMMAND_QUEUE_OFFSET + READ_POINTER;
        nonzero_bytes += !is_command_queue_header && memory.bytes[offset] != 0 ? 1 : 0;
    }

    EXPECT_EQ(nonzero_bytes, 0u);
}

// Each message carries NVIDIA's headers, its payload and zeros to its last element's end over an older message's
// bytes, and starts in the element after the previous message's last, one element holding up to 4016 payload bytes
TEST(nvidia_message_queues, messages_are_framed_as_nvidia_frames_them) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    string::memset(memory.element(0), STALE_BYTE, 4 * ELEMENT_BYTES);
    queue_test_payload short_payload(13);
    queue_test_payload one_element_payload(FIRST_ELEMENT_PAYLOAD_BYTES);
    queue_test_payload two_element_payload(FIRST_ELEMENT_PAYLOAD_BYTES + 1);
    ASSERT_NOT_NULL(short_payload.bytes);
    ASSERT_NOT_NULL(one_element_payload.bytes);
    ASSERT_NOT_NULL(two_element_payload.bytes);

    EXPECT_EQ(queues.send(FUNCTION, short_payload.bytes, short_payload.count), OK);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 1u);
    EXPECT_EQ(queues.send(FUNCTION, one_element_payload.bytes, one_element_payload.count), OK);
    EXPECT_EQ(queues.send(FUNCTION, two_element_payload.bytes, two_element_payload.count), OK);
    expect_message(memory, 0, 0, short_payload.bytes, short_payload.count);
    expect_message(memory, 1, 1, one_element_payload.bytes, one_element_payload.count);
    expect_message(memory, 2, 2, two_element_payload.bytes, two_element_payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 4u);
}

// A message that reaches past the last element continues in the first, the write pointer wraps with it, and the
// sequence number keeps counting past 0xFF as the ring goes around
TEST(nvidia_message_queues, message_wraps_to_the_first_element) {
    // Enough turns of the ring that the last message's sequence number passes 0xFF
    static constexpr uint32_t RING_TURNS = 4;
    static constexpr uint32_t EARLIER_MESSAGES = RING_TURNS * ELEMENT_COUNT + ELEMENT_COUNT - 2;

    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    queue_test_payload payload(2 * ELEMENT_BYTES + 100);
    ASSERT_NOT_NULL(payload.bytes);

    for (uint32_t i = 0; i < EARLIER_MESSAGES; i++) {
        ASSERT_EQ(queues.send(FUNCTION, nullptr, 0), OK);
        memory.write(COMMAND_QUEUE_OFFSET + READ_POINTER, (i + 1) % ELEMENT_COUNT);
    }

    string::memset(memory.element(0), STALE_BYTE, ELEMENT_BYTES);
    EXPECT_EQ(queues.send(FUNCTION, payload.bytes, payload.count), OK);
    expect_message(memory, ELEMENT_COUNT - 2, EARLIER_MESSAGES, payload.bytes, payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 1u);
}

// A message ending at the last element's end writes nothing past the command queue
TEST(nvidia_message_queues, message_ending_at_the_ring_end_writes_nothing_past_it) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    string::memset(memory.bytes + STATUS_QUEUE_OFFSET, STALE_BYTE, QUEUE_BYTES);
    queue_test_payload payload(FIRST_ELEMENT_PAYLOAD_BYTES);
    ASSERT_NOT_NULL(payload.bytes);

    for (uint32_t i = 0; i < ELEMENT_COUNT - 1; i++) {
        ASSERT_EQ(queues.send(FUNCTION, nullptr, 0), OK);
        memory.write(COMMAND_QUEUE_OFFSET + READ_POINTER, i + 1);
    }

    EXPECT_EQ(queues.send(FUNCTION, payload.bytes, payload.count), OK);
    expect_message(memory, ELEMENT_COUNT - 1, ELEMENT_COUNT - 1, payload.bytes, payload.count);
    EXPECT_TRUE(is_stale(memory.bytes + STATUS_QUEUE_OFFSET, QUEUE_BYTES));
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 0u);
}

// A message fits only in elements the firmware's read pointer has passed, counted around the ring's end, less the
// one kept free, and a refused message writes nothing and takes no sequence number
TEST(nvidia_message_queues, message_needs_elements_the_firmware_has_read_past) {
    static constexpr uint32_t LONG_MESSAGES = 4;
    static constexpr uint32_t LONG_MESSAGE_ELEMENTS = 15;
    static constexpr uint32_t FILLED_ELEMENTS = LONG_MESSAGES * LONG_MESSAGE_ELEMENTS;

    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    string::memset(memory.element(FILLED_ELEMENTS), STALE_BYTE, ELEMENT_BYTES);
    queue_test_payload long_payload(LONG_MESSAGE_ELEMENTS * ELEMENT_BYTES - MESSAGE_PAYLOAD);
    queue_test_payload three_element_payload(2 * ELEMENT_BYTES);
    queue_test_payload two_element_payload(ELEMENT_BYTES);
    ASSERT_NOT_NULL(long_payload.bytes);
    ASSERT_NOT_NULL(three_element_payload.bytes);
    ASSERT_NOT_NULL(two_element_payload.bytes);

    for (uint32_t i = 0; i < LONG_MESSAGES; i++) {
        ASSERT_EQ(queues.send(FUNCTION, long_payload.bytes, long_payload.count), OK);
    }

    EXPECT_EQ(queues.send(FUNCTION, three_element_payload.bytes, three_element_payload.count), ERR_QUEUE_FULL);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), FILLED_ELEMENTS);
    EXPECT_TRUE(is_stale(memory.element(FILLED_ELEMENTS), ELEMENT_BYTES));

    EXPECT_EQ(queues.send(FUNCTION, two_element_payload.bytes, two_element_payload.count), OK);
    expect_message(memory, FILLED_ELEMENTS, LONG_MESSAGES, two_element_payload.bytes, two_element_payload.count);
    EXPECT_EQ(queues.send(FUNCTION, nullptr, 0), ERR_QUEUE_FULL);

    memory.write(COMMAND_QUEUE_OFFSET + READ_POINTER, 1);
    EXPECT_EQ(queues.send(FUNCTION, nullptr, 0), OK);
    expect_message(memory, ELEMENT_COUNT - 1, LONG_MESSAGES + 1, nullptr, 0);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 0u);
    EXPECT_EQ(queues.send(FUNCTION, nullptr, 0), ERR_QUEUE_FULL);

    memory.write(COMMAND_QUEUE_OFFSET + READ_POINTER, 3);
    EXPECT_EQ(queues.send(FUNCTION, three_element_payload.bytes, three_element_payload.count), ERR_QUEUE_FULL);
    EXPECT_EQ(queues.send(FUNCTION, two_element_payload.bytes, two_element_payload.count), OK);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 2u);
}

// A firmware read pointer at or past the ring's 63 elements leaves no room, as NVIDIA's count of free elements
// treats it, even where its low 8 or 16 bits alone would leave room, while one at the last element leaves room
TEST(nvidia_message_queues, read_pointer_past_the_ring_leaves_no_room) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);

    memory.write(COMMAND_QUEUE_OFFSET + READ_POINTER, ELEMENT_COUNT);
    EXPECT_EQ(queues.send(FUNCTION, nullptr, 0), ERR_QUEUE_FULL);
    memory.write(COMMAND_QUEUE_OFFSET + READ_POINTER, 0x100);
    EXPECT_EQ(queues.send(FUNCTION, nullptr, 0), ERR_QUEUE_FULL);
    memory.write(COMMAND_QUEUE_OFFSET + READ_POINTER, 0x10000);
    EXPECT_EQ(queues.send(FUNCTION, nullptr, 0), ERR_QUEUE_FULL);
    memory.write(COMMAND_QUEUE_OFFSET + READ_POINTER, 0xFFFFFFFF);
    EXPECT_EQ(queues.send(FUNCTION, nullptr, 0), ERR_QUEUE_FULL);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 0u);

    memory.write(COMMAND_QUEUE_OFFSET + READ_POINTER, ELEMENT_COUNT - 1);
    EXPECT_EQ(queues.send(FUNCTION, nullptr, 0), OK);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 1u);
}

// NVIDIA's largest payload, 64 KiB less 80 bytes of headers, fills 16 elements and nothing past them, and larger
// payloads are refused unwritten, even where the size's low 16 bits or its sum with the headers would fit
TEST(nvidia_message_queues, payload_limit_is_nvidias) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    string::memset(memory.element(MESSAGE_ELEMENTS), STALE_BYTE, ELEMENT_BYTES);
    queue_test_payload largest_payload(PAYLOAD_MAX_BYTES);
    queue_test_payload oversized_payload(PAYLOAD_MAX_BYTES + 1);
    ASSERT_NOT_NULL(largest_payload.bytes);
    ASSERT_NOT_NULL(oversized_payload.bytes);

    EXPECT_EQ(MESSAGE_PAYLOAD_MAX_BYTES, PAYLOAD_MAX_BYTES);
    EXPECT_EQ(queues.send(FUNCTION, largest_payload.bytes, largest_payload.count), OK);
    expect_message(memory, 0, 0, largest_payload.bytes, largest_payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), MESSAGE_ELEMENTS);
    EXPECT_TRUE(is_stale(memory.element(MESSAGE_ELEMENTS), ELEMENT_BYTES));

    EXPECT_EQ(queues.send(FUNCTION, oversized_payload.bytes, oversized_payload.count), ERR_TOO_LARGE);
    EXPECT_EQ(queues.send(FUNCTION, oversized_payload.bytes, 0x1000D), ERR_TOO_LARGE);
    EXPECT_EQ(queues.send(FUNCTION, oversized_payload.bytes, 0u - MESSAGE_PAYLOAD + 13), ERR_TOO_LARGE);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), MESSAGE_ELEMENTS);
    EXPECT_TRUE(is_stale(memory.element(MESSAGE_ELEMENTS), ELEMENT_BYTES));
}

// With a swapping firmware the driver keeps its read pointer in the command queue's header page, zeroed by the first
// link only and moved past each message, and `send` reads the firmware's where the status queue's header places it
TEST(nvidia_message_queues, swapped_read_pointers_lie_in_the_queues_their_readers_write) {
    static constexpr status_queue_test_header HEADER = MOVED_HEADER;

    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    queue_test_payload payload(13);
    status_test_message message(1);
    ASSERT_NOT_NULL(payload.bytes);
    ASSERT_NOT_NULL(message.bytes);

    HEADER.write_to(memory);
    message.frame(1, 0, payload.bytes, payload.count);
    message.store(memory, HEADER, 0);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 1);
    memory.write(COMMAND_QUEUE_OFFSET + READ_POINTER, STALE_WORD);
    memory.write(STATUS_QUEUE_OFFSET + READ_POINTER, 0);
    memory.write(STATUS_QUEUE_OFFSET + HEADER.read_pointer_offset, ELEMENT_COUNT);
    expect_refused(queues, memory, HEADER, ERR_NOT_LINKED);

    EXPECT_EQ(queues.link_status_queue(), OK);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), 0u);
    EXPECT_EQ(memory.read(STATUS_QUEUE_OFFSET + HEADER.read_pointer_offset), ELEMENT_COUNT);
    EXPECT_EQ(queues.send(FUNCTION, nullptr, 0), ERR_QUEUE_FULL);
    memory.write(STATUS_QUEUE_OFFSET + HEADER.read_pointer_offset, 0);
    EXPECT_EQ(queues.send(FUNCTION, nullptr, 0), OK);

    expect_received(queues, payload.bytes, payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), 1u);
    EXPECT_EQ(memory.read(STATUS_QUEUE_OFFSET + HEADER.read_pointer_offset), 0u);

    message.frame(1, 1, payload.bytes, payload.count);
    message.store(memory, HEADER, 1);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 2);
    EXPECT_EQ(queues.link_status_queue(), OK);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), 1u);
    expect_received(queues, payload.bytes, payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), 2u);
}

// With a firmware that does not swap, the driver keeps its read pointer in the status queue at the offset its header
// gives, and `send` keeps reading the firmware's from the command queue's header page
TEST(nvidia_message_queues, unswapped_read_pointers_lie_in_the_queues_their_readers_read) {
    status_queue_test_header header = MOVED_HEADER;
    header.flags = 0;

    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    queue_test_payload payload(13);
    status_test_message message(1);
    ASSERT_NOT_NULL(payload.bytes);
    ASSERT_NOT_NULL(message.bytes);

    header.write_to(memory);
    message.frame(1, 0, payload.bytes, payload.count);
    message.store(memory, header, 0);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 1);
    memory.write(STATUS_QUEUE_OFFSET + header.read_pointer_offset, STALE_WORD);
    memory.write(COMMAND_QUEUE_OFFSET + READ_POINTER, ELEMENT_COUNT);

    EXPECT_EQ(queues.link_status_queue(), OK);
    EXPECT_EQ(memory.read(STATUS_QUEUE_OFFSET + header.read_pointer_offset), 0u);
    EXPECT_EQ(queues.send(FUNCTION, nullptr, 0), ERR_QUEUE_FULL);

    expect_received(queues, payload.bytes, payload.count);
    EXPECT_EQ(memory.read(STATUS_QUEUE_OFFSET + header.read_pointer_offset), 1u);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), ELEMENT_COUNT);
}

// Each fails a check NVIDIA's driver makes, some only above 16 bits, or a rule of the driver's own: elements at
// 0xFFFFF000, which NVIDIA's 32-bit sum accepts, or a read pointer misaligned or not wholly before the first element
static constexpr status_queue_test_header REFUSED_STATUS_QUEUE_HEADERS[] = {
    {.version = 1},
    {.version = 0x10000},
    {.bytes = QUEUE_BYTES - PAGE_BYTES},
    {.bytes = QUEUE_BYTES + PAGE_BYTES},
    {.bytes = 0x10000000 + QUEUE_BYTES},
    {.element_bytes = ELEMENT_BYTES / 2},
    {.element_bytes = ELEMENT_BYTES * 2},
    {.element_bytes = 0x10000 + ELEMENT_BYTES},
    {.element_count = ELEMENT_COUNT - 1},
    {.element_count = ELEMENT_COUNT + 1},
    {.element_count = 0x10000 + ELEMENT_COUNT},
    {.elements_offset = 0x10000 + ELEMENTS_OFFSET},
    {.element_count = 0, .elements_offset = QUEUE_BYTES - ELEMENT_BYTES + 1},
    {.element_count = (QUEUE_BYTES - 0xFFFFF000) / ELEMENT_BYTES, .elements_offset = 0xFFFFF000},
    {.read_pointer_offset = READ_POINTER - 4},
    {.read_pointer_offset = READ_POINTER + 2},
    {.read_pointer_offset = ELEMENTS_OFFSET},
    {.read_pointer_offset = 0x10000 + READ_POINTER},
    {.read_pointer_offset = 0xFFFFFFFC},
    {.element_count = (QUEUE_BYTES - 0x1002) / ELEMENT_BYTES, .read_pointer_offset = 0x1000, .elements_offset = 0x1002},
    {.element_count = (QUEUE_BYTES - 0x800) / ELEMENT_BYTES, .read_pointer_offset = 0x800, .elements_offset = 0x800},
};

// Headers that pass every check above at its edge, where only the flags' swap bit decides whether read pointers swap
static constexpr status_queue_test_header ACCEPTED_STATUS_QUEUE_HEADERS[] = {
    {},
    {.flags = 0xFFFFFFFF},
    {.flags = ~SWAPS_READ_POINTERS},
    {.flags = 0, .read_pointer_offset = ELEMENTS_OFFSET - 4},
    {.element_count = 1, .elements_offset = QUEUE_BYTES - ELEMENT_BYTES},
    {.element_count = (QUEUE_BYTES - 0x2000) / ELEMENT_BYTES, .read_pointer_offset = 0x1FFC, .elements_offset = 0x2000},
    MOVED_HEADER,
};

// The status queue links only once the firmware has written a header that passes the link's checks, a refused header
// writes nothing, and an accepted one gets the driver's read pointer set to 0 where the swap rule places it
TEST(nvidia_message_queues, status_queue_links_only_to_a_header_passing_every_check) {
    static constexpr uint32_t HEADER_TAIL_AND_FIRST_ELEMENT_BYTES = 2 * PAGE_BYTES - READ_POINTER;

    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    string::memset(memory.bytes + COMMAND_QUEUE_OFFSET + READ_POINTER, STALE_BYTE, HEADER_TAIL_AND_FIRST_ELEMENT_BYTES);
    string::memset(memory.bytes + STATUS_QUEUE_OFFSET + READ_POINTER, STALE_BYTE, HEADER_TAIL_AND_FIRST_ELEMENT_BYTES);

    EXPECT_EQ(queues.link_status_queue(), ERR_NOT_LINKED);
    for (const status_queue_test_header& header : REFUSED_STATUS_QUEUE_HEADERS) {
        header.write_to(memory);
        EXPECT_EQ(queues.link_status_queue(), ERR_NOT_LINKED);
    }

    expect_refused(queues, memory, {}, ERR_NOT_LINKED);
    EXPECT_TRUE(is_stale(memory.bytes + COMMAND_QUEUE_OFFSET + READ_POINTER, HEADER_TAIL_AND_FIRST_ELEMENT_BYTES));
    EXPECT_TRUE(is_stale(memory.bytes + STATUS_QUEUE_OFFSET + READ_POINTER, HEADER_TAIL_AND_FIRST_ELEMENT_BYTES));

    for (const status_queue_test_header& header : ACCEPTED_STATUS_QUEUE_HEADERS) {
        message_queues accepting_queues(memory.bytes, DMA_ADDRESS);
        header.write_to(memory);
        memory.write(header.driver_read_pointer(), STALE_WORD);
        memory.write(header.firmware_read_pointer(), STALE_WORD);

        EXPECT_EQ(accepting_queues.link_status_queue(), OK);
        EXPECT_EQ(memory.read(header.driver_read_pointer()), 0u);
        EXPECT_EQ(memory.read(header.firmware_read_pointer()), STALE_WORD);
    }
}

// Messages are received from where the header places the elements, 4016 payload bytes filling one and 65456 all 16,
// one is refused while the write pointer has passed only part of it, and bytes past the checksum's words go unchecked
TEST(nvidia_message_queues, messages_framed_as_nvidias_driver_reads_them_are_received) {
    static constexpr status_queue_test_header HEADER = MOVED_HEADER;
    static constexpr uint32_t PAST_CHECKED_WORDS = checked_bytes_for(MESSAGE_PAYLOAD + 13);

    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    queue_test_payload short_payload(13);
    queue_test_payload one_element_payload(FIRST_ELEMENT_PAYLOAD_BYTES);
    queue_test_payload two_element_payload(FIRST_ELEMENT_PAYLOAD_BYTES + 1);
    queue_test_payload largest_payload(PAYLOAD_MAX_BYTES);
    status_test_message message(MESSAGE_ELEMENTS);
    ASSERT_NOT_NULL(short_payload.bytes);
    ASSERT_NOT_NULL(one_element_payload.bytes);
    ASSERT_NOT_NULL(two_element_payload.bytes);
    ASSERT_NOT_NULL(largest_payload.bytes);
    ASSERT_NOT_NULL(message.bytes);

    HEADER.write_to(memory);
    message.frame(1, 0, short_payload.bytes, short_payload.count);
    message.bytes[PAST_CHECKED_WORDS] = STALE_BYTE;
    message.store(memory, HEADER, 0);
    message.frame(1, 1, one_element_payload.bytes, one_element_payload.count);
    message.store(memory, HEADER, 1);
    message.frame(2, 2, two_element_payload.bytes, two_element_payload.count);
    message.store(memory, HEADER, 2);
    message.frame(2, 3, short_payload.bytes, short_payload.count);
    message.store(memory, HEADER, 4);
    message.frame(MESSAGE_ELEMENTS, 4, largest_payload.bytes, largest_payload.count);
    message.store(memory, HEADER, 6);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 5);
    ASSERT_EQ(queues.link_status_queue(), OK);

    expect_received(queues, short_payload.bytes, short_payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), 1u);
    expect_received(queues, one_element_payload.bytes, one_element_payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), 2u);
    expect_received(queues, two_element_payload.bytes, two_element_payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), 4u);
    expect_refused(queues, memory, HEADER, ERR_BAD_MESSAGE);

    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 6 + MESSAGE_ELEMENTS);
    expect_received(queues, short_payload.bytes, short_payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), 6u);
    expect_received(queues, largest_payload.bytes, largest_payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), 6 + MESSAGE_ELEMENTS);
    expect_refused(queues, memory, HEADER, ERR_QUEUE_EMPTY);
}

// A message wrapping from the last element to the first is refused while only the last is written, the read pointer
// wraps with the header's count, and with a sequence number past 0xFF due, one matching only its low 8 bits is refused
TEST(nvidia_message_queues, message_wrapping_to_the_first_element_is_received) {
    static constexpr status_queue_test_header HEADER = MOVED_HEADER;
    static constexpr uint32_t RING_TURNS = 4;
    static constexpr uint32_t EARLIER_MESSAGES = RING_TURNS * HEADER.element_count + HEADER.element_count - 1;

    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    queue_test_payload payload(FIRST_ELEMENT_PAYLOAD_BYTES + 1);
    status_test_message message(2);
    ASSERT_NOT_NULL(payload.bytes);
    ASSERT_NOT_NULL(message.bytes);

    HEADER.write_to(memory);
    ASSERT_EQ(queues.link_status_queue(), OK);

    for (uint32_t i = 0; i < EARLIER_MESSAGES; i++) {
        message.frame(1, i, nullptr, 0);
        message.store(memory, HEADER, i % HEADER.element_count);
        memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, (i + 1) % HEADER.element_count);

        received_message out = {};
        ASSERT_EQ(queues.receive(out, nullptr, 0), OK);
    }

    message.frame(2, EARLIER_MESSAGES, payload.bytes, payload.count);
    message.store(memory, HEADER, HEADER.element_count - 1);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 0);
    expect_refused(queues, memory, HEADER, ERR_BAD_MESSAGE);

    message.frame(2, EARLIER_MESSAGES & 0xFF, payload.bytes, payload.count);
    message.store(memory, HEADER, HEADER.element_count - 1);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 1);
    expect_refused(queues, memory, HEADER, ERR_BAD_MESSAGE);

    message.frame(2, EARLIER_MESSAGES, payload.bytes, payload.count);
    message.store(memory, HEADER, HEADER.element_count - 1);
    expect_received(queues, payload.bytes, payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), 1u);
}

// Stores the message from element 0 on, moves the write pointer to `written_elements`, and checks that receiving
// refuses it as a bad message
static void expect_message_refused(
    message_queues& queues,
    fake_queue_memory& memory,
    const status_queue_test_header& header,
    const status_test_message& message,
    uint32_t written_elements
) {
    message.store(memory, header, 0);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, written_elements);
    expect_refused(queues, memory, header, ERR_BAD_MESSAGE);
}

// A message failing a check, some only above 16 bits, or too large for the caller is refused in place with nothing
// written, and one passing every check written over it is then received
TEST(nvidia_message_queues, message_failing_a_check_is_refused_in_place) {
    static constexpr status_queue_test_header HEADER = {};

    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    queue_test_payload payload(13);
    queue_test_payload two_element_payload(FIRST_ELEMENT_PAYLOAD_BYTES + 1);
    queue_test_payload oversized_payload(PAYLOAD_MAX_BYTES + 1);
    status_test_message message(MESSAGE_ELEMENTS + 1);
    ASSERT_NOT_NULL(payload.bytes);
    ASSERT_NOT_NULL(two_element_payload.bytes);
    ASSERT_NOT_NULL(oversized_payload.bytes);
    ASSERT_NOT_NULL(message.bytes);

    HEADER.write_to(memory);
    ASSERT_EQ(queues.link_status_queue(), OK);

    message.frame(1, 0, payload.bytes, payload.count);
    message.set(MESSAGE_ELEMENT_COUNT, 0);
    message.seal(MESSAGE_PAYLOAD + payload.count);
    expect_message_refused(queues, memory, HEADER, message, 1);

    message.set(MESSAGE_ELEMENT_COUNT, MESSAGE_ELEMENTS + 1);
    message.seal(MESSAGE_PAYLOAD + payload.count);
    expect_message_refused(queues, memory, HEADER, message, ELEMENT_COUNT - 1);

    message.set(MESSAGE_ELEMENT_COUNT, 0x10001);
    message.seal(MESSAGE_PAYLOAD + payload.count);
    expect_message_refused(queues, memory, HEADER, message, ELEMENT_COUNT - 1);

    message.frame(1, 0, nullptr, 0);
    message.set(MESSAGE_RPC_BYTES, RPC_HEADER_BYTES - 1);
    message.seal(MESSAGE_PAYLOAD - 1);
    expect_message_refused(queues, memory, HEADER, message, 1);

    message.set(MESSAGE_RPC_BYTES, 0x10000 + RPC_HEADER_BYTES);
    message.seal(MESSAGE_PAYLOAD);
    expect_message_refused(queues, memory, HEADER, message, 1);

    message.frame(2, 0, two_element_payload.bytes, two_element_payload.count);
    message.set(MESSAGE_ELEMENT_COUNT, 1);
    message.seal(MESSAGE_PAYLOAD + two_element_payload.count);
    expect_message_refused(queues, memory, HEADER, message, 2);

    message.frame(MESSAGE_ELEMENTS + 1, 0, oversized_payload.bytes, oversized_payload.count);
    message.set(MESSAGE_ELEMENT_COUNT, MESSAGE_ELEMENTS);
    message.seal(MESSAGE_PAYLOAD + oversized_payload.count);
    expect_message_refused(queues, memory, HEADER, message, MESSAGE_ELEMENTS + 1);

    message.frame(1, 0, payload.bytes, payload.count);
    message.bytes[MESSAGE_PAYLOAD] ^= 0x01;
    expect_message_refused(queues, memory, HEADER, message, 1);

    message.frame(1, 0, payload.bytes, payload.count);
    message.bytes[checked_bytes_for(MESSAGE_PAYLOAD + payload.count) - 1] = 0x01;
    expect_message_refused(queues, memory, HEADER, message, 1);

    message.frame(MESSAGE_ELEMENTS, 0, oversized_payload.bytes, PAYLOAD_MAX_BYTES);
    message.bytes[MESSAGE_ELEMENTS * ELEMENT_BYTES - 1] ^= 0x01;
    expect_message_refused(queues, memory, HEADER, message, MESSAGE_ELEMENTS);

    message.frame(1, 1, payload.bytes, payload.count);
    expect_message_refused(queues, memory, HEADER, message, 1);

    message.frame(1, 0x10000, payload.bytes, payload.count);
    expect_message_refused(queues, memory, HEADER, message, 1);

    message.frame(1, 0, payload.bytes, payload.count);
    message.store(memory, HEADER, 0);
    expect_refused(queues, memory, HEADER, ERR_TOO_LARGE, payload.count - 1);
    expect_received(queues, payload.bytes, payload.count, payload.count);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + READ_POINTER), 1u);
}

// With one of two messages received, a write pointer moved back to the read pointer, or to or past the ring's end
// even where its low 8 or 16 bits alone name written elements, shows no message, and the second shows once it moves on
TEST(nvidia_message_queues, write_pointer_moved_back_or_past_the_ring_shows_no_message) {
    static constexpr status_queue_test_header RINGS[] = {{}, MOVED_HEADER};

    queue_test_payload payload(13);
    status_test_message message(1);
    ASSERT_NOT_NULL(payload.bytes);
    ASSERT_NOT_NULL(message.bytes);

    for (const status_queue_test_header& header : RINGS) {
        fake_queue_memory memory;
        ASSERT_NOT_NULL(memory.bytes);
        message_queues queues(memory.bytes, DMA_ADDRESS);
        uint32_t write_pointers_showing_nothing[] = {1, header.element_count, 0x102, 0x10002, 0xFFFFFFFF};

        header.write_to(memory);
        message.frame(1, 0, payload.bytes, payload.count);
        message.store(memory, header, 0);
        message.frame(1, 1, payload.bytes, payload.count);
        message.store(memory, header, 1);
        memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 2);
        ASSERT_EQ(queues.link_status_queue(), OK);
        expect_received(queues, payload.bytes, payload.count);

        for (uint32_t write_pointer : write_pointers_showing_nothing) {
            memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, write_pointer);
            expect_refused(queues, memory, header, ERR_QUEUE_EMPTY);
        }

        memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, header.element_count - 1);
        expect_received(queues, payload.bytes, payload.count);
        EXPECT_EQ(memory.read(header.driver_read_pointer()), 2u);
    }
}
