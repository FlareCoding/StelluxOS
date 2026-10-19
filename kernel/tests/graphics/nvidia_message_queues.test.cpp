// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/cpu_sequencer.h"
#include "drivers/graphics/nvidia/gsp.h"
#include "drivers/graphics/nvidia/gsp_info.h"
#include "drivers/graphics/nvidia/gsp_unload.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_message_queues);

using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_NOT_LINKED;
using drivers::nvidia::ERR_QUEUE_EMPTY;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::ERR_TOO_LARGE;
using drivers::nvidia::FUNCTION_GSP_INIT_DONE;
using drivers::nvidia::MESSAGE_PAYLOAD_MAX_BYTES;
using drivers::nvidia::OK;
using drivers::nvidia::PASSED_MESSAGES_LOGGED;
using drivers::nvidia::QUEUE_MEMORY_BYTES;
using drivers::nvidia::RECEIVE_TIMEOUT_NS;
using drivers::nvidia::RECEIVE_TRIES;
using drivers::nvidia::STATUS_QUEUE_LINK_TIMEOUT_NS;
using drivers::nvidia::confirm_message_version;
using drivers::nvidia::cpu_sequencer_context;
using drivers::nvidia::gsp;
using drivers::nvidia::link_status_queue_within;
using drivers::nvidia::message_queues;
using drivers::nvidia::read_static_configuration;
using drivers::nvidia::receive_awaited;
using drivers::nvidia::receive_within;
using drivers::nvidia::received_message;
using drivers::nvidia::registers;
using drivers::nvidia::static_configuration;
using drivers::nvidia::unload_gsp_firmware;
using drivers::nvidia::video_memory_region;

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

// A private result other than `RESULT` and above 16 bits, so a private result cut to 16 bits or read from the result's
// field shows
static constexpr uint32_t PRIVATE_RESULT = 0x2468ACE1;

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

    // Frames a message of `FUNCTION`, `RESULT` and `PRIVATE_RESULT` filling `element_count` elements, zeros following
    // the payload
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

// NVIDIA's numbers for the ready message and the CPU sequencer's, restated here so the tests hold the wait to NVIDIA's
// numbers rather than to the driver's constants
static constexpr uint32_t NVIDIA_GSP_INIT_DONE     = 0x1001;
static constexpr uint32_t NVIDIA_RUN_CPU_SEQUENCER = 0x1002;

// The words of the CPU sequencer's save area, and the opcodes of its register write and register poll
static constexpr uint32_t SEQUENCER_SAVE_AREA_WORDS = 8;
static constexpr uint32_t SEQUENCER_WRITE           = 0;
static constexpr uint32_t SEQUENCER_POLL            = 2;

// The registers of a fake BAR0 the sequencer polls and then writes, and the bit the poll waits for
static constexpr uint32_t TEST_BAR0_BYTES  = 0x400;
static constexpr uint32_t POLLED_REGISTER  = 0x100;
static constexpr uint32_t WRITTEN_REGISTER = 0x200;
static constexpr uint32_t POLLED_BIT       = 0x80000000;
static constexpr uint32_t WRITTEN_VALUE    = 0x13579BDF;
static constexpr uint32_t POLL_TIMEOUT_US  = 1000;

// A zeroed fake of the part of BAR0 the sequencer's steps reach
struct awaited_test_bar0 {
    uint8_t*  bytes = static_cast<uint8_t*>(heap::uzalloc(TEST_BAR0_BYTES));
    registers regs{reinterpret_cast<uintptr_t>(bytes)};

    ~awaited_test_bar0() { heap::ufree(bytes); }

    uint32_t read(uint32_t offset) const { return *reinterpret_cast<uint32_t*>(bytes + offset); }
    void write(uint32_t offset, uint32_t value) { *reinterpret_cast<uint32_t*>(bytes + offset) = value; }
};

// A CPU sequencer's payload in NVIDIA's layout that polls `POLLED_REGISTER` for `POLLED_BIT`, then writes
// `WRITTEN_REGISTER`, its buffer one word larger than its steps, the fewest NVIDIA accepts
struct poll_then_write_sequencer {
    static constexpr uint32_t STEP_WORDS = 9;

    uint32_t buffer_words                         = STEP_WORDS + 1;
    uint32_t step_words                           = STEP_WORDS;
    uint32_t save_area[SEQUENCER_SAVE_AREA_WORDS] = {};

    uint32_t poll_opcode     = SEQUENCER_POLL;
    uint32_t poll_register   = POLLED_REGISTER;
    uint32_t poll_mask       = POLLED_BIT;
    uint32_t poll_value      = POLLED_BIT;
    uint32_t poll_timeout_us = POLL_TIMEOUT_US;
    uint32_t poll_error      = 0;

    uint32_t write[3] = {SEQUENCER_WRITE, WRITTEN_REGISTER, WRITTEN_VALUE};
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
    set(MESSAGE_PRIVATE_RESULT, PRIVATE_RESULT);
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
// `FUNCTION`, `RESULT`, `PRIVATE_RESULT` and this payload, with nothing written past the payload
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

    received_message out = {STALE_WORD, STALE_WORD, STALE_WORD, STALE_WORD};
    EXPECT_EQ(queues.receive(out, buffer, payload_capacity), OK);
    EXPECT_EQ(out.function, FUNCTION);
    EXPECT_EQ(out.result, RESULT);
    EXPECT_EQ(out.private_result, PRIVATE_RESULT);
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
        received_message out = {STALE_WORD, STALE_WORD, STALE_WORD, STALE_WORD};
        EXPECT_EQ(queues.receive(out, buffer, payload_capacity), error);
        EXPECT_EQ(out.function, STALE_WORD);
        EXPECT_EQ(out.result, STALE_WORD);
        EXPECT_EQ(out.private_result, STALE_WORD);
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

// NVIDIA's link timeout, its poll timeout (its default of 4 s and half as long again) and the most times it reads one
// message in a row, restated here so the tests hold the waits to NVIDIA's values rather than to the driver's constants
static constexpr uint64_t NVIDIA_STATUS_QUEUE_LINK_TIMEOUT_NS = 4000000000;
static constexpr uint64_t NVIDIA_RECEIVE_TIMEOUT_NS           = 6000000000;
static constexpr uint32_t NVIDIA_RECEIVE_TRIES                = 3;

// The link and the poll get NVIDIA's timeouts, and a message gets at most NVIDIA's number of reads in a row
TEST(nvidia_message_queues, waits_are_nvidias) {
    EXPECT_EQ(STATUS_QUEUE_LINK_TIMEOUT_NS, NVIDIA_STATUS_QUEUE_LINK_TIMEOUT_NS);
    EXPECT_EQ(RECEIVE_TIMEOUT_NS, NVIDIA_RECEIVE_TIMEOUT_NS);
    EXPECT_EQ(RECEIVE_TRIES, NVIDIA_RECEIVE_TRIES);
}

// A wait that must run out gets the short timeout and lasts at least that long, a wait an error must end at once gets
// the long timeout, and each timed wait ends within the limit, far short of the long timeout
static constexpr uint64_t SHORT_TIMEOUT_NS   = 1000000;
static constexpr uint64_t LONG_TIMEOUT_NS    = 1000000000;
static constexpr uint64_t WAIT_TIME_LIMIT_NS = 100000000;

// The link waits out its timeout while the firmware has written no header, and since it is tried at least once, a
// header already written links with no time to wait
TEST(nvidia_message_queues, status_queue_link_waits_out_its_timeout) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(link_status_queue_within(queues, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, WAIT_TIME_LIMIT_NS);

    status_queue_test_header{}.write_to(memory);
    EXPECT_EQ(link_status_queue_within(queues, 0), OK);
}

// Room for the payload `receive_waits_out_its_timeout_only_on_an_empty_queue` receives
static constexpr uint32_t WAIT_BUFFER_BYTES = 64;

// A wait on an empty queue lasts its timeout and a waiting message is received with no time to wait, while an unlinked
// queue, a refused message or too little room ends a long wait at once, leaving the message in place
TEST(nvidia_message_queues, receive_waits_out_its_timeout_only_on_an_empty_queue) {
    static constexpr status_queue_test_header HEADER = MOVED_HEADER;

    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    queue_test_payload payload(13);
    status_test_message message(1);
    ASSERT_NOT_NULL(payload.bytes);
    ASSERT_NOT_NULL(message.bytes);
    uint8_t buffer[WAIT_BUFFER_BYTES];
    received_message out = {STALE_WORD, STALE_WORD, STALE_WORD, STALE_WORD};

    uint64_t start = clock::now_ns();
    EXPECT_EQ(receive_within(queues, out, buffer, sizeof(buffer), LONG_TIMEOUT_NS), ERR_NOT_LINKED);
    EXPECT_LT(clock::now_ns() - start, WAIT_TIME_LIMIT_NS);

    HEADER.write_to(memory);
    ASSERT_EQ(queues.link_status_queue(), OK);
    start = clock::now_ns();
    EXPECT_EQ(receive_within(queues, out, buffer, sizeof(buffer), SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, WAIT_TIME_LIMIT_NS);

    message.frame(1, 1, payload.bytes, payload.count);
    message.store(memory, HEADER, 0);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 1);
    start = clock::now_ns();
    EXPECT_EQ(receive_within(queues, out, buffer, sizeof(buffer), LONG_TIMEOUT_NS), ERR_BAD_MESSAGE);
    EXPECT_LT(clock::now_ns() - start, WAIT_TIME_LIMIT_NS);

    message.frame(1, 0, payload.bytes, payload.count);
    message.store(memory, HEADER, 0);
    start = clock::now_ns();
    EXPECT_EQ(receive_within(queues, out, buffer, payload.count - 1, LONG_TIMEOUT_NS), ERR_TOO_LARGE);
    EXPECT_LT(clock::now_ns() - start, WAIT_TIME_LIMIT_NS);
    EXPECT_EQ(out.function, STALE_WORD);
    EXPECT_EQ(out.result, STALE_WORD);
    EXPECT_EQ(out.payload_bytes, STALE_WORD);
    EXPECT_EQ(memory.read(HEADER.driver_read_pointer()), 0u);

    EXPECT_EQ(receive_within(queues, out, buffer, sizeof(buffer), 0), OK);
    EXPECT_EQ(out.function, FUNCTION);
    EXPECT_EQ(out.result, RESULT);
    EXPECT_EQ(out.payload_bytes, payload.count);
    EXPECT_EQ(string::memcmp(buffer, payload.bytes, payload.count), 0);
    EXPECT_EQ(memory.read(HEADER.driver_read_pointer()), 1u);
}

// `FUNCTION_GSP_INIT_DONE` holds NVIDIA's number for the ready message
TEST(nvidia_message_queues, ready_message_number_is_nvidias) {
    EXPECT_EQ(FUNCTION_GSP_INIT_DONE, NVIDIA_GSP_INIT_DONE);
}

// Room for any payload the tests of `receive_awaited` receive, the sequencer's the largest
static constexpr uint32_t AWAITED_BUFFER_BYTES = 128;

// Stores a message of `function` with a result of 0, this private result and this payload in element `index` of the
// moved status queue, numbered `index`
static void store_function_message(
    fake_queue_memory& memory,
    status_test_message& message,
    uint32_t index,
    uint32_t function,
    const void* payload,
    uint32_t payload_bytes,
    uint32_t private_result = 0
) {
    message.frame(1, index, static_cast<const uint8_t*>(payload), payload_bytes);
    message.set(MESSAGE_FUNCTION, function);
    message.set(MESSAGE_RESULT, 0);
    message.set(MESSAGE_PRIVATE_RESULT, private_result);
    message.seal(MESSAGE_PAYLOAD + payload_bytes);
    message.store(memory, MOVED_HEADER, index);
}

// Waits for the ready message until `deadline_ns`, the sequencer's waits bound by a deadline the long timeout later,
// so a wait that ends at the sequencer's deadline instead of its own fails a test
static int32_t await_ready(
    message_queues& queues,
    awaited_test_bar0& bar0,
    received_message& out,
    uint8_t (&buffer)[AWAITED_BUFFER_BYTES],
    uint64_t deadline_ns
) {
    cpu_sequencer_context context = {
        .register_bytes          = TEST_BAR0_BYTES,
        .boot0                   = 0,
        .libos_arguments_address = 0,
        .app_version             = 0,
        .ready_timeout_ns        = 0,
        .deadline_ns             = deadline_ns + LONG_TIMEOUT_NS,
    };

    return receive_awaited(
        queues,
        bar0.regs,
        context,
        FUNCTION_GSP_INIT_DONE,
        out,
        buffer,
        sizeof(buffer),
        deadline_ns
    );
}

// Where the backend below drops the polled bit, and the text it keeps
static registers* s_dropping_registers = nullptr;
static char       s_logged_text[4096]  = {};
static uint32_t   s_logged_bytes       = 0;

// Drops the polled bit at any log line, as the firmware holds the bit for less time than a log line can take, and
// keeps the line's text
static void drop_polled_bit_at_any_line(const char* data, size_t len) {
    s_dropping_registers->write(POLLED_REGISTER, 0);
    for (size_t i = 0; i < len && s_logged_bytes < sizeof(s_logged_text); i++) {
        s_logged_text[s_logged_bytes++] = data[i];
    }
}

static constexpr log::backend POLLED_BIT_DROPPING_BACKEND = {.write = drop_polled_bit_at_any_line};

static bool logged_text_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_logged_bytes; start++) {
        if (string::memcmp(s_logged_text + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

static uint32_t logged_line_count() {
    uint32_t count = 0;
    for (uint32_t i = 0; i + 1 < s_logged_bytes; i++) {
        if (s_logged_text[i] == '\r' && s_logged_text[i + 1] == '\n') {
            count++;
        }
    }

    return count;
}

// The sequencer runs before any log line, as the firmware holds the bit its first poll waits for only briefly from
// its send, while a message before it is logged only after the run and the ready message's payload is returned
TEST(nvidia_message_queues, receive_awaited_runs_the_sequencer_before_any_log_line) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    awaited_test_bar0 bar0;
    queue_test_payload earlier_payload(13);
    queue_test_payload ready_payload(29);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(earlier_payload.bytes);
    ASSERT_NOT_NULL(ready_payload.bytes);
    ASSERT_NOT_NULL(message.bytes);
    poll_then_write_sequencer sequencer;
    uint8_t buffer[AWAITED_BUFFER_BYTES];
    received_message out = {STALE_WORD, STALE_WORD, STALE_WORD, STALE_WORD};

    MOVED_HEADER.write_to(memory);
    message.frame(1, 0, earlier_payload.bytes, earlier_payload.count);
    message.store(memory, MOVED_HEADER, 0);
    store_function_message(memory, message, 1, NVIDIA_RUN_CPU_SEQUENCER, &sequencer, sizeof(sequencer));
    store_function_message(memory, message, 2, NVIDIA_GSP_INIT_DONE, ready_payload.bytes, ready_payload.count);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 3);
    ASSERT_EQ(queues.link_status_queue(), OK);
    bar0.write(POLLED_REGISTER, POLLED_BIT);

    s_dropping_registers = &bar0.regs;
    s_logged_bytes = 0;
    log::set_backend(&POLLED_BIT_DROPPING_BACKEND);
    int32_t rc = await_ready(queues, bar0, out, buffer, clock::now_ns() + LONG_TIMEOUT_NS);
    log::set_backend(nullptr);

    EXPECT_EQ(rc, OK);
    EXPECT_EQ(bar0.read(WRITTEN_REGISTER), WRITTEN_VALUE);
    EXPECT_TRUE(logged_text_contains("function 0x12345"));
    EXPECT_EQ(out.function, NVIDIA_GSP_INIT_DONE);
    EXPECT_EQ(out.result, 0u);
    EXPECT_EQ(out.payload_bytes, ready_payload.count);
    EXPECT_EQ(string::memcmp(buffer, ready_payload.bytes, ready_payload.count), 0);
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), 3u);
}

// A sequencer that fails stops at its failed step and the wait goes on to the first of two ready messages, while a
// deadline already passed still reads the queue once
TEST(nvidia_message_queues, receive_awaited_goes_on_past_a_failed_sequencer) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    awaited_test_bar0 bar0;
    queue_test_payload first_payload(13);
    queue_test_payload second_payload(29);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(first_payload.bytes);
    ASSERT_NOT_NULL(second_payload.bytes);
    ASSERT_NOT_NULL(message.bytes);
    poll_then_write_sequencer sequencer;
    uint8_t buffer[AWAITED_BUFFER_BYTES];
    received_message out = {};

    MOVED_HEADER.write_to(memory);
    store_function_message(memory, message, 0, NVIDIA_RUN_CPU_SEQUENCER, &sequencer, sizeof(sequencer));
    store_function_message(memory, message, 1, NVIDIA_GSP_INIT_DONE, first_payload.bytes, first_payload.count);
    store_function_message(memory, message, 2, NVIDIA_GSP_INIT_DONE, second_payload.bytes, second_payload.count);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 3);
    ASSERT_EQ(queues.link_status_queue(), OK);

    EXPECT_EQ(await_ready(queues, bar0, out, buffer, clock::now_ns() + LONG_TIMEOUT_NS), OK);
    EXPECT_EQ(bar0.read(WRITTEN_REGISTER), 0u);
    EXPECT_EQ(out.payload_bytes, first_payload.count);
    EXPECT_EQ(string::memcmp(buffer, first_payload.bytes, first_payload.count), 0);
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), 2u);

    EXPECT_EQ(await_ready(queues, bar0, out, buffer, 0), OK);
    EXPECT_EQ(out.payload_bytes, second_payload.count);
    EXPECT_EQ(string::memcmp(buffer, second_payload.bytes, second_payload.count), 0);
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), 3u);
}

// A wait whose message never comes takes the messages before it and runs out at its deadline, and a refused message
// ends a long wait at once, each leaving the caller's message unchanged
TEST(nvidia_message_queues, receive_awaited_ends_at_its_deadline_or_a_refused_message) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    awaited_test_bar0 bar0;
    queue_test_payload payload(13);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(payload.bytes);
    ASSERT_NOT_NULL(message.bytes);
    uint8_t buffer[AWAITED_BUFFER_BYTES];
    received_message out = {STALE_WORD, STALE_WORD, STALE_WORD, STALE_WORD};

    MOVED_HEADER.write_to(memory);
    message.frame(1, 0, payload.bytes, payload.count);
    message.store(memory, MOVED_HEADER, 0);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 1);
    ASSERT_EQ(queues.link_status_queue(), OK);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(await_ready(queues, bar0, out, buffer, start + SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, WAIT_TIME_LIMIT_NS);
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), 1u);

    // The same message again, numbered 0 where 1 is due
    message.store(memory, MOVED_HEADER, 1);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 2);
    start = clock::now_ns();
    EXPECT_EQ(await_ready(queues, bar0, out, buffer, start + LONG_TIMEOUT_NS), ERR_BAD_MESSAGE);
    EXPECT_LT(clock::now_ns() - start, WAIT_TIME_LIMIT_NS);
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), 1u);
    EXPECT_EQ(out.function, STALE_WORD);
    EXPECT_EQ(out.result, STALE_WORD);
    EXPECT_EQ(out.payload_bytes, STALE_WORD);
}

// A deadline the sequencer's poll outlasts, and that poll's timeout, three times as long
static constexpr uint64_t OUTLASTED_DEADLINE_NS      = 100000000;
static constexpr uint32_t OUTLASTING_POLL_TIMEOUT_US = 300000;

// A sequencer whose poll outlasts the wait's deadline runs to the poll's own timeout, after which the wait reads the
// empty queue once and ends, as the run counts against the deadline
TEST(nvidia_message_queues, receive_awaited_counts_a_sequencer_run_against_its_deadline) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    awaited_test_bar0 bar0;
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(message.bytes);
    poll_then_write_sequencer sequencer;
    sequencer.poll_timeout_us = OUTLASTING_POLL_TIMEOUT_US;
    uint8_t buffer[AWAITED_BUFFER_BYTES];
    received_message out = {};

    MOVED_HEADER.write_to(memory);
    store_function_message(memory, message, 0, NVIDIA_RUN_CPU_SEQUENCER, &sequencer, sizeof(sequencer));
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 1);
    ASSERT_EQ(queues.link_status_queue(), OK);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(await_ready(queues, bar0, out, buffer, start + OUTLASTED_DEADLINE_NS), ERR_TIMEOUT);
    uint64_t elapsed = clock::now_ns() - start;

    uint64_t poll_timeout_ns = OUTLASTING_POLL_TIMEOUT_US * 1000ULL;
    EXPECT_GE(elapsed, poll_timeout_ns);
    EXPECT_LT(elapsed, poll_timeout_ns + WAIT_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(WRITTEN_REGISTER), 0u);
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), 1u);
}

// Of more messages passed over than the wait logs, the first `PASSED_MESSAGES_LOGGED` are logged and the rest counted,
// each line naming the function awaited
TEST(nvidia_message_queues, receive_awaited_logs_the_first_passed_messages_and_counts_the_rest) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    awaited_test_bar0 bar0;
    queue_test_payload payload(4);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(payload.bytes);
    ASSERT_NOT_NULL(message.bytes);
    uint8_t buffer[AWAITED_BUFFER_BYTES];
    received_message out = {};

    MOVED_HEADER.write_to(memory);
    uint32_t passed_count = PASSED_MESSAGES_LOGGED + 1;
    for (uint32_t i = 0; i < passed_count; i++) {
        store_function_message(memory, message, i, FUNCTION + i, payload.bytes, payload.count);
    }

    store_function_message(memory, message, passed_count, NVIDIA_GSP_INIT_DONE, payload.bytes, payload.count);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, passed_count + 1);
    ASSERT_EQ(queues.link_status_queue(), OK);

    s_dropping_registers = &bar0.regs;
    s_logged_bytes = 0;
    log::set_backend(&POLLED_BIT_DROPPING_BACKEND);
    int32_t rc = await_ready(queues, bar0, out, buffer, clock::now_ns() + LONG_TIMEOUT_NS);
    log::set_backend(nullptr);

    EXPECT_EQ(rc, OK);
    EXPECT_TRUE(logged_text_contains("the wait for function 0x1001 passed over a message of function 0x12354"));
    EXPECT_FALSE(logged_text_contains("function 0x12355"));
    EXPECT_TRUE(logged_text_contains(
        "the wait for function 0x1001 passed over more messages from the GSP's firmware, not logged: 1\r\n"
    ));
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), passed_count + 1);
}

// A wait that runs out after passing over a message logs that message and then its own timeout, and no other line
TEST(nvidia_message_queues, timed_out_wait_logs_the_passed_message_then_its_timeout) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, DMA_ADDRESS);
    awaited_test_bar0 bar0;
    queue_test_payload payload(4);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(payload.bytes);
    ASSERT_NOT_NULL(message.bytes);
    uint8_t buffer[AWAITED_BUFFER_BYTES];
    received_message out = {};

    MOVED_HEADER.write_to(memory);
    store_function_message(memory, message, 0, FUNCTION, payload.bytes, payload.count);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 1);
    ASSERT_EQ(queues.link_status_queue(), OK);

    s_dropping_registers = &bar0.regs;
    s_logged_bytes = 0;
    log::set_backend(&POLLED_BIT_DROPPING_BACKEND);
    int32_t rc = await_ready(queues, bar0, out, buffer, clock::now_ns() + SHORT_TIMEOUT_NS);
    log::set_backend(nullptr);

    EXPECT_EQ(rc, ERR_TIMEOUT);
    EXPECT_EQ(logged_line_count(), 2u);
    EXPECT_TRUE(logged_text_contains("the wait for function 0x1001 passed over a message of function 0x12345"));
    EXPECT_TRUE(logged_text_contains("sent no message of function 0x1001 by the wait's deadline\r\n"));
}

// NVIDIA's number for the request to unload and the size of its arguments, restated here so the tests hold the
// unload to NVIDIA's numbers
static constexpr uint32_t NVIDIA_UNLOADING_GUEST_DRIVER = 0x2F;
static constexpr uint32_t UNLOAD_ARGUMENTS_BYTES        = 8;

// The doorbell of the resource manager's queue, the GSP falcon's first mailbox, and what NVIDIA's driver reads in
// that mailbox once the GSP's processor is suspended
static constexpr uint32_t GSP_QUEUE_HEAD0     = 0x110C00;
static constexpr uint32_t GSP_FALCON_MAILBOX0 = 0x110040;
static constexpr uint32_t PROCESSOR_SUSPENDED = 0x80000000;

// A zeroed fake of BAR0, all 16 MiB of it, so a register misplaced in another engine's range fails a test
struct request_test_bar0 {
    static constexpr uint32_t BYTES = 0x1000000;

    uint8_t*  bytes = static_cast<uint8_t*>(heap::uzalloc(BYTES));
    registers regs{reinterpret_cast<uintptr_t>(bytes)};

    ~request_test_bar0() { heap::ufree(bytes); }

    uint32_t read(uint32_t offset) const { return *reinterpret_cast<uint32_t*>(bytes + offset); }
    void write(uint32_t offset, uint32_t value) { *reinterpret_cast<uint32_t*>(bytes + offset) = value; }
};

// Stores a reply to the request with this result and 8 zero bytes of payload in element `index` of the moved status
// queue, numbered `index`
static void store_unload_reply(
    fake_queue_memory& memory,
    status_test_message& message,
    uint32_t index,
    uint32_t result
) {
    uint8_t arguments[UNLOAD_ARGUMENTS_BYTES] = {};
    message.frame(1, index, arguments, sizeof(arguments));
    message.set(MESSAGE_FUNCTION, NVIDIA_UNLOADING_GUEST_DRIVER);
    message.set(MESSAGE_RESULT, result);
    message.seal(MESSAGE_PAYLOAD + sizeof(arguments));
    message.store(memory, MOVED_HEADER, index);
}

// Starts `conversation` on the fake memory and the fake BAR0, with a sequencer context that reaches all of that BAR0
static int32_t start_conversation(gsp& conversation, fake_queue_memory& memory, request_test_bar0& bar0) {
    cpu_sequencer_context context = {
        .register_bytes          = request_test_bar0::BYTES,
        .boot0                   = 0,
        .libos_arguments_address = 0,
        .app_version             = 0,
        .ready_timeout_ns        = 0,
        .deadline_ns             = 0,
    };

    return conversation.start(bar0.regs, memory.bytes, DMA_ADDRESS, context);
}

// Links the moved status queue holding `reply_count` replies, the first with `first_result` and the rest with 0
static void link_unload_replies(
    fake_queue_memory& memory,
    gsp& conversation,
    status_test_message& message,
    uint32_t reply_count,
    uint32_t first_result
) {
    MOVED_HEADER.write_to(memory);
    for (uint32_t i = 0; i < reply_count; i++) {
        store_unload_reply(memory, message, i, i == 0 ? first_result : 0);
    }

    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, reply_count);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);
}

// Only the firmware's success status, 0, translates to OK, and each nonzero status tried gives ERR_FIRMWARE, from
// the lowest bit set alone to the highest
TEST(nvidia_message_queues, only_the_firmwares_success_status_translates_to_ok) {
    EXPECT_EQ(gsp::to_error(0), OK);
    EXPECT_EQ(gsp::to_error(1), ERR_FIRMWARE);
    EXPECT_EQ(gsp::to_error(0x100), ERR_FIRMWARE);
    EXPECT_EQ(gsp::to_error(0x10000), ERR_FIRMWARE);
    EXPECT_EQ(gsp::to_error(0x80000000), ERR_FIRMWARE);
    EXPECT_EQ(gsp::to_error(RESULT), ERR_FIRMWARE);
}

// `start` lays the queues out at the card address it is given, which the page table lists for the first page
TEST(nvidia_message_queues, start_lays_the_queues_out_at_the_card_address_given) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    registers no_registers(0);
    cpu_sequencer_context unused_sequencer_context = {};
    gsp conversation;

    ASSERT_EQ(conversation.start(no_registers, memory.bytes, DMA_ADDRESS, unused_sequencer_context), OK);

    EXPECT_EQ(*reinterpret_cast<uint64_t*>(memory.bytes), DMA_ADDRESS);
}

// The link waits out its own timeout while the status queue's header stays unwritten, and the wait for a message the
// firmware sends unasked waits out its own on a queue that stays empty
TEST(nvidia_message_queues, link_and_await_wait_out_their_timeouts) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    received_message out = {STALE_WORD, STALE_WORD, STALE_WORD, STALE_WORD};

    uint64_t start = clock::now_ns();
    EXPECT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t link_elapsed = clock::now_ns() - start;
    EXPECT_GE(link_elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(link_elapsed, WAIT_TIME_LIMIT_NS);

    MOVED_HEADER.write_to(memory);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);
    start = clock::now_ns();
    EXPECT_EQ(conversation.await(FUNCTION_GSP_INIT_DONE, out, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t await_elapsed = clock::now_ns() - start;

    EXPECT_GE(await_elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(await_elapsed, WAIT_TIME_LIMIT_NS);
    EXPECT_EQ(out.function, STALE_WORD);
}

// Links the moved status queue holding a sequencer in element 0 and, in element 1, a message of `function` whose
// payload holds the bytes of `payload`
static void link_sequencer_then_message(
    fake_queue_memory& memory,
    gsp& conversation,
    status_test_message& message,
    const poll_then_write_sequencer& sequencer,
    uint32_t function,
    const queue_test_payload& payload
) {
    MOVED_HEADER.write_to(memory);
    store_function_message(memory, message, 0, NVIDIA_RUN_CPU_SEQUENCER, &sequencer, sizeof(sequencer));
    store_function_message(memory, message, 1, function, payload.bytes, payload.count);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 2);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);
}

// A sequencer stored ahead of a request's reply runs before any log line, as the firmware holds the bit the sequencer's
// first poll waits for only briefly after sending the sequencer, and the request then takes its reply
TEST(nvidia_message_queues, request_runs_a_sequencer_ahead_of_its_reply_before_any_log_line) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    queue_test_payload arguments(24);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(arguments.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    poll_then_write_sequencer sequencer;
    link_sequencer_then_message(memory, conversation, message, sequencer, FUNCTION, arguments);
    bar0.write(POLLED_REGISTER, POLLED_BIT);
    bar0.write(GSP_QUEUE_HEAD0, STALE_WORD);

    received_message reply = {};

    s_dropping_registers = &bar0.regs;
    s_logged_bytes = 0;
    log::set_backend(&POLLED_BIT_DROPPING_BACKEND);
    int32_t rc = conversation.request(FUNCTION, arguments.bytes, arguments.count, 0, reply, LONG_TIMEOUT_NS);
    log::set_backend(nullptr);

    const uint8_t* request = memory.element(0);
    EXPECT_EQ(rc, OK);
    EXPECT_EQ(bar0.read(WRITTEN_REGISTER), WRITTEN_VALUE);
    EXPECT_EQ(field_at(request, MESSAGE_FUNCTION), FUNCTION);
    EXPECT_EQ(string::memcmp(request + MESSAGE_PAYLOAD, arguments.bytes, arguments.count), 0);
    EXPECT_EQ(bar0.read(GSP_QUEUE_HEAD0), 0u);
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), 2u);
}

// A poll mask over every bit of the doorbell, so only the request's write of 0 to it meets the poll
static constexpr uint32_t WHOLE_REGISTER_MASK = 0xFFFFFFFF;

// Before its wait begins, a request tells the firmware that the request waits, so a sequencer stored ahead of the reply
// that polls the doorbell for 0 finds it written
TEST(nvidia_message_queues, request_tells_the_firmware_before_its_wait) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    queue_test_payload arguments(24);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(arguments.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    poll_then_write_sequencer sequencer;
    sequencer.poll_register = GSP_QUEUE_HEAD0;
    sequencer.poll_mask = WHOLE_REGISTER_MASK;
    sequencer.poll_value = 0;
    link_sequencer_then_message(memory, conversation, message, sequencer, FUNCTION, arguments);
    bar0.write(GSP_QUEUE_HEAD0, STALE_WORD);

    received_message reply = {};
    EXPECT_EQ(conversation.request(FUNCTION, arguments.bytes, arguments.count, 0, reply, LONG_TIMEOUT_NS), OK);
    EXPECT_EQ(bar0.read(WRITTEN_REGISTER), WRITTEN_VALUE);
}

// A request the full command queue has no room for fails at once with the queue's error, leaving its reply unwritten,
// and the firmware is never told that the request waits
TEST(nvidia_message_queues, request_to_a_full_queue_fails_before_telling_the_firmware) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    MOVED_HEADER.write_to(memory);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);
    memory.write(MOVED_HEADER.firmware_read_pointer(), 1);
    bar0.write(GSP_QUEUE_HEAD0, STALE_WORD);

    received_message reply = {STALE_WORD, STALE_WORD, STALE_WORD, STALE_WORD};

    uint64_t start = clock::now_ns();
    EXPECT_EQ(conversation.request(FUNCTION, nullptr, 0, 0, reply, LONG_TIMEOUT_NS), ERR_QUEUE_FULL);
    EXPECT_LT(clock::now_ns() - start, WAIT_TIME_LIMIT_NS);

    EXPECT_EQ(bar0.read(GSP_QUEUE_HEAD0), STALE_WORD);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 0u);
    EXPECT_EQ(reply.function, STALE_WORD);
}

// A poll timeout long enough for the test to tell a poll that runs to it from one that ends at its first check
static constexpr uint32_t UNMET_POLL_TIMEOUT_US = 50000;

// A request gives the sequencer a deadline of its own rather than the reply's, so a poll that is never met runs to its
// own timeout past the 1 ms reply deadline, and the reply, read once more after that deadline, is still taken
TEST(nvidia_message_queues, request_gives_the_sequencer_a_deadline_of_its_own) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    queue_test_payload arguments(24);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(arguments.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    poll_then_write_sequencer sequencer;
    sequencer.poll_timeout_us = UNMET_POLL_TIMEOUT_US;
    link_sequencer_then_message(memory, conversation, message, sequencer, FUNCTION, arguments);

    received_message reply = {};

    uint64_t start = clock::now_ns();
    EXPECT_EQ(conversation.request(FUNCTION, arguments.bytes, arguments.count, 0, reply, SHORT_TIMEOUT_NS), OK);
    uint64_t elapsed = clock::now_ns() - start;

    uint64_t poll_timeout_ns = UNMET_POLL_TIMEOUT_US * 1000ULL;
    EXPECT_GE(elapsed, poll_timeout_ns);
    EXPECT_LT(elapsed, poll_timeout_ns + WAIT_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(WRITTEN_REGISTER), 0u);
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), 2u);
}

// The wait for a message the firmware sends unasked also gives the sequencer a deadline of its own rather than the
// wait's, so a poll that is never met runs to its own timeout past the 1 ms wait, and the ready message is still taken
TEST(nvidia_message_queues, await_gives_the_sequencer_a_deadline_of_its_own) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    queue_test_payload ready_payload(16);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(ready_payload.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    poll_then_write_sequencer sequencer;
    sequencer.poll_timeout_us = UNMET_POLL_TIMEOUT_US;
    link_sequencer_then_message(memory, conversation, message, sequencer, NVIDIA_GSP_INIT_DONE, ready_payload);
    received_message out = {STALE_WORD, STALE_WORD, STALE_WORD, STALE_WORD};

    uint64_t start = clock::now_ns();
    EXPECT_EQ(conversation.await(FUNCTION_GSP_INIT_DONE, out, SHORT_TIMEOUT_NS), OK);
    uint64_t elapsed = clock::now_ns() - start;

    uint64_t poll_timeout_ns = UNMET_POLL_TIMEOUT_US * 1000ULL;
    EXPECT_GE(elapsed, poll_timeout_ns);
    EXPECT_LT(elapsed, poll_timeout_ns + WAIT_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(WRITTEN_REGISTER), 0u);
    EXPECT_EQ(out.function, NVIDIA_GSP_INIT_DONE);
    EXPECT_EQ(out.payload_bytes, ready_payload.count);
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), 2u);
}

// The request goes out with NVIDIA's number and 8 bytes of arguments, all 0, the doorbell is written, and the wait
// passes over a message before the reply, which with the suspended value in the mailbox completes the unload
TEST(nvidia_message_queues, unload_is_requested_and_its_reply_and_the_suspend_awaited) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    queue_test_payload passed_payload(40);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(passed_payload.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    MOVED_HEADER.write_to(memory);
    store_function_message(memory, message, 0, FUNCTION, passed_payload.bytes, passed_payload.count);
    store_unload_reply(memory, message, 1, 0);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 2);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);
    bar0.write(GSP_QUEUE_HEAD0, STALE_WORD);
    bar0.write(GSP_FALCON_MAILBOX0, PROCESSOR_SUSPENDED);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(unload_gsp_firmware(conversation, bar0.regs, LONG_TIMEOUT_NS, LONG_TIMEOUT_NS), OK);
    EXPECT_LT(clock::now_ns() - start, WAIT_TIME_LIMIT_NS);

    const uint8_t* request = memory.element(0);
    uint32_t nonzero_argument_bytes = 0;
    for (uint32_t i = 0; i < UNLOAD_ARGUMENTS_BYTES; i++) {
        nonzero_argument_bytes += request[MESSAGE_PAYLOAD + i] != 0 ? 1 : 0;
    }

    EXPECT_EQ(field_at(request, MESSAGE_FUNCTION), NVIDIA_UNLOADING_GUEST_DRIVER);
    EXPECT_EQ(field_at(request, MESSAGE_RPC_BYTES), RPC_HEADER_BYTES + UNLOAD_ARGUMENTS_BYTES);
    EXPECT_EQ(nonzero_argument_bytes, 0u);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 1u);
    EXPECT_EQ(bar0.read(GSP_QUEUE_HEAD0), 0u);
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), 2u);
}

// A reply whose result is not 0 fails the unload only after the wait for the suspend has run out, as NVIDIA's driver
// waits whatever the reply, and a good reply followed by no suspend fails with the wait's timeout
TEST(nvidia_message_queues, unload_waits_for_the_suspend_whatever_the_reply) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    link_unload_replies(memory, conversation, message, 2, RESULT);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(unload_gsp_firmware(conversation, bar0.regs, LONG_TIMEOUT_NS, SHORT_TIMEOUT_NS), ERR_FIRMWARE);
    uint64_t refused_elapsed = clock::now_ns() - start;
    EXPECT_GE(refused_elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(refused_elapsed, WAIT_TIME_LIMIT_NS);

    start = clock::now_ns();
    EXPECT_EQ(unload_gsp_firmware(conversation, bar0.regs, LONG_TIMEOUT_NS, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    EXPECT_LT(clock::now_ns() - start, WAIT_TIME_LIMIT_NS);
    EXPECT_EQ(memory.read(MOVED_HEADER.driver_read_pointer()), 2u);
}

// With no reply by its deadline the unload fails with the reply wait's timeout only after a suspend wait ten times as
// long has run out, and with the processor suspended the reply wait still takes its own timeout, not the suspend's
TEST(nvidia_message_queues, unanswered_unload_still_waits_for_the_suspend) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    link_unload_replies(memory, conversation, message, 0, 0);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(unload_gsp_firmware(conversation, bar0.regs, SHORT_TIMEOUT_NS, 10 * SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t unanswered_elapsed = clock::now_ns() - start;
    EXPECT_GE(unanswered_elapsed, 11 * SHORT_TIMEOUT_NS);
    EXPECT_LT(unanswered_elapsed, WAIT_TIME_LIMIT_NS);

    bar0.write(GSP_FALCON_MAILBOX0, PROCESSOR_SUSPENDED);
    start = clock::now_ns();
    EXPECT_EQ(unload_gsp_firmware(conversation, bar0.regs, SHORT_TIMEOUT_NS, LONG_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t suspended_elapsed = clock::now_ns() - start;
    EXPECT_GE(suspended_elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(suspended_elapsed, WAIT_TIME_LIMIT_NS);
    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 2u);
}

// The reply's fields and as much of its payload as fits the room reach the caller, its result alone failing the
// request, the arguments sent before the copy, and a wait ending without the reply leaves the fields and the room alone
TEST(nvidia_message_queues, request_hands_its_caller_the_reply_and_the_payload_that_fits) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    queue_test_payload payload(40);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(payload.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    MOVED_HEADER.write_to(memory);
    message.frame(1, 0, payload.bytes, payload.count);
    message.store(memory, MOVED_HEADER, 0);
    store_function_message(memory, message, 1, FUNCTION, payload.bytes, payload.count, PRIVATE_RESULT);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 2);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);
    uint8_t cut_payload[64];
    uint8_t whole_payload[64];
    uint8_t unanswered_payload[64];
    string::memset(cut_payload, STALE_BYTE, sizeof(cut_payload));
    string::memset(whole_payload, STALE_BYTE, sizeof(whole_payload));
    string::memset(unanswered_payload, STALE_BYTE, sizeof(unanswered_payload));
    received_message refused = {};
    received_message accepted = {};
    received_message unanswered = {STALE_WORD, STALE_WORD, STALE_WORD, STALE_WORD};

    EXPECT_EQ(conversation.request(FUNCTION, cut_payload, 0, 24, refused, LONG_TIMEOUT_NS), ERR_FIRMWARE);
    EXPECT_EQ(conversation.request(FUNCTION, whole_payload, 24, 64, accepted, LONG_TIMEOUT_NS), OK);
    EXPECT_EQ(conversation.request(FUNCTION, unanswered_payload, 0, 64, unanswered, SHORT_TIMEOUT_NS), ERR_TIMEOUT);

    const uint8_t* second_request = memory.element(1);
    EXPECT_EQ(refused.function, FUNCTION);
    EXPECT_EQ(refused.result, RESULT);
    EXPECT_EQ(refused.private_result, PRIVATE_RESULT);
    EXPECT_EQ(refused.payload_bytes, 40u);
    EXPECT_EQ(string::memcmp(cut_payload, payload.bytes, 24), 0);
    EXPECT_TRUE(is_stale(cut_payload + 24, sizeof(cut_payload) - 24));
    EXPECT_EQ(accepted.result, 0u);
    EXPECT_EQ(accepted.private_result, PRIVATE_RESULT);
    EXPECT_EQ(accepted.payload_bytes, 40u);
    EXPECT_TRUE(is_stale(second_request + MESSAGE_PAYLOAD, 24));
    EXPECT_EQ(string::memcmp(whole_payload, payload.bytes, 40), 0);
    EXPECT_TRUE(is_stale(whole_payload + 40, sizeof(whole_payload) - 40));
    EXPECT_EQ(unanswered.function, STALE_WORD);
    EXPECT_EQ(unanswered.private_result, STALE_WORD);
    EXPECT_EQ(unanswered.payload_bytes, STALE_WORD);
    EXPECT_TRUE(is_stale(unanswered_payload, sizeof(unanswered_payload)));
}

// NVIDIA's number for the first request sent after the ready message and the layout of its arguments, restated here
// so the tests hold the request to NVIDIA's number and layout rather than to the driver's constants
static constexpr uint32_t NVIDIA_SET_GUEST_SYSTEM_INFO = 0x01;
static constexpr uint32_t GUEST_SYSTEM_INFO_BYTES      = 792;
static constexpr uint32_t GUEST_VGX_MAJOR              = 0;
static constexpr uint32_t GUEST_VGX_MINOR              = 4;
static constexpr uint32_t GUEST_VGX_VERSION_BYTES      = 8;
static constexpr uint32_t GUEST_DRIVER_VERSION_LENGTH  = 8;
static constexpr uint32_t GUEST_BRANCH_LENGTH          = 12;
static constexpr uint32_t GUEST_TITLE_LENGTH           = 16;
static constexpr uint32_t GUEST_DRIVER_VERSION         = 24;
static constexpr uint32_t GUEST_STRING_BYTES           = 256;

// NVIDIA's VGX version for this firmware's messages and the version string its driver sends as its own
static constexpr uint32_t VGX_MAJOR = 0x23;
static constexpr uint32_t VGX_MINOR = 0x05;
static constexpr char NVIDIA_DRIVER_VERSION[] = "535.183.01";

static void put_word(uint8_t* bytes, uint32_t offset, uint32_t value) {
    string::memcpy(bytes + offset, &value, sizeof(value));
}

// Fills `info` with the guest system information the driver sends, giving the VGX version `major`.`minor`
static void fill_guest_system_info(uint8_t (&info)[GUEST_SYSTEM_INFO_BYTES], uint32_t major, uint32_t minor) {
    string::memset(info, 0, sizeof(info));
    put_word(info, GUEST_VGX_MAJOR, major);
    put_word(info, GUEST_VGX_MINOR, minor);
    put_word(info, GUEST_DRIVER_VERSION_LENGTH, GUEST_STRING_BYTES);
    put_word(info, GUEST_BRANCH_LENGTH, GUEST_STRING_BYTES);
    put_word(info, GUEST_TITLE_LENGTH, GUEST_STRING_BYTES);
    string::memcpy(info + GUEST_DRIVER_VERSION, NVIDIA_DRIVER_VERSION, sizeof(NVIDIA_DRIVER_VERSION));
}

// The guest system information goes out in NVIDIA's layout with the VGX version, the 256-byte lengths, the firmware's
// version string as the driver's and zeros elsewhere, and a reply with private result 0 and that version accepts it
TEST(nvidia_message_queues, message_version_goes_out_in_nvidias_layout_and_its_echo_is_accepted) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    uint8_t expected[GUEST_SYSTEM_INFO_BYTES];
    fill_guest_system_info(expected, VGX_MAJOR, VGX_MINOR);
    MOVED_HEADER.write_to(memory);
    store_function_message(memory, message, 0, NVIDIA_SET_GUEST_SYSTEM_INFO, expected, sizeof(expected));
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 1);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);

    EXPECT_EQ(confirm_message_version(conversation, LONG_TIMEOUT_NS), OK);

    const uint8_t* request = memory.element(0);
    EXPECT_EQ(field_at(request, MESSAGE_FUNCTION), NVIDIA_SET_GUEST_SYSTEM_INFO);
    EXPECT_EQ(field_at(request, MESSAGE_RPC_BYTES), RPC_HEADER_BYTES + GUEST_SYSTEM_INFO_BYTES);
    EXPECT_EQ(string::memcmp(request + MESSAGE_PAYLOAD, expected, sizeof(expected)), 0);
}

// Versions off by one either way in either field, or equal to the driver's only in their low 16 bits
static constexpr uint32_t OTHER_VERSIONS[][2] = {
    {VGX_MAJOR + 1, VGX_MINOR},
    {VGX_MAJOR - 1, VGX_MINOR},
    {VGX_MAJOR, VGX_MINOR + 1},
    {VGX_MAJOR, VGX_MINOR - 1},
    {VGX_MAJOR + 0x10000, VGX_MINOR},
    {VGX_MAJOR, VGX_MINOR + 0x10000},
};

static constexpr uint32_t OTHER_VERSION_COUNT = sizeof(OTHER_VERSIONS) / sizeof(OTHER_VERSIONS[0]);

// A failed result, or a private result other than 0 (0x10000 included) whatever the reply's length or version, fails
// with no request resent, as do a version off by one or equal only in its low 16 bits and 7 payload bytes, not 8
TEST(nvidia_message_queues, message_version_refused_changed_or_unstated_fails) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    uint8_t info[GUEST_SYSTEM_INFO_BYTES];
    uint32_t cut_bytes = GUEST_VGX_VERSION_BYTES - 1;
    MOVED_HEADER.write_to(memory);
    fill_guest_system_info(info, VGX_MAJOR, VGX_MINOR);
    store_function_message(memory, message, 0, NVIDIA_SET_GUEST_SYSTEM_INFO, info, sizeof(info), PRIVATE_RESULT);
    store_function_message(memory, message, 1, NVIDIA_SET_GUEST_SYSTEM_INFO, info, sizeof(info), 0x10000);
    store_function_message(memory, message, 2, NVIDIA_SET_GUEST_SYSTEM_INFO, info, cut_bytes, PRIVATE_RESULT);
    store_function_message(memory, message, 3, NVIDIA_SET_GUEST_SYSTEM_INFO, info, cut_bytes);
    store_function_message(memory, message, 4, NVIDIA_SET_GUEST_SYSTEM_INFO, info, GUEST_VGX_VERSION_BYTES);
    store_function_message(memory, message, 5, NVIDIA_SET_GUEST_SYSTEM_INFO, info, sizeof(info));
    message.set(MESSAGE_RESULT, RESULT);
    message.seal(MESSAGE_PAYLOAD + sizeof(info));
    message.store(memory, MOVED_HEADER, 5);
    fill_guest_system_info(info, VGX_MAJOR - 1, VGX_MINOR);
    store_function_message(memory, message, 6, NVIDIA_SET_GUEST_SYSTEM_INFO, info, sizeof(info), PRIVATE_RESULT);
    for (uint32_t i = 0; i < OTHER_VERSION_COUNT; i++) {
        fill_guest_system_info(info, OTHER_VERSIONS[i][0], OTHER_VERSIONS[i][1]);
        store_function_message(memory, message, 7 + i, NVIDIA_SET_GUEST_SYSTEM_INFO, info, sizeof(info));
    }

    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 7 + OTHER_VERSION_COUNT);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);

    EXPECT_EQ(confirm_message_version(conversation, LONG_TIMEOUT_NS), ERR_FIRMWARE);
    EXPECT_EQ(confirm_message_version(conversation, LONG_TIMEOUT_NS), ERR_FIRMWARE);
    EXPECT_EQ(confirm_message_version(conversation, LONG_TIMEOUT_NS), ERR_FIRMWARE);
    EXPECT_EQ(confirm_message_version(conversation, LONG_TIMEOUT_NS), ERR_BAD_MESSAGE);
    EXPECT_EQ(confirm_message_version(conversation, LONG_TIMEOUT_NS), OK);
    EXPECT_EQ(confirm_message_version(conversation, LONG_TIMEOUT_NS), ERR_FIRMWARE);
    EXPECT_EQ(confirm_message_version(conversation, LONG_TIMEOUT_NS), ERR_FIRMWARE);
    for (uint32_t i = 0; i < OTHER_VERSION_COUNT; i++) {
        EXPECT_EQ(confirm_message_version(conversation, LONG_TIMEOUT_NS), ERR_FIRMWARE);
    }

    EXPECT_EQ(memory.read(COMMAND_QUEUE_OFFSET + HEADER_WRITE_POINTER), 7 + OTHER_VERSION_COUNT);
}

// NVIDIA's number for the request for the static configuration, its size and the layout of its table of video memory
// regions, restated here so the tests hold the request and the table to NVIDIA's layout rather than to the driver's
static constexpr uint32_t NVIDIA_GET_GSP_STATIC_INFO = 0x41;
static constexpr uint32_t STATIC_INFO_BYTES          = 2168;
static constexpr uint32_t REGION_COUNT_OFFSET        = 840;
static constexpr uint32_t REGION_PADDING_OFFSET      = 844;
static constexpr uint32_t REGION_ENTRIES_OFFSET      = 848;
static constexpr uint32_t REGION_ENTRY_BYTES         = 48;
static constexpr uint32_t REGIONS_MAX                = 16;
static constexpr uint32_t REGION_TABLE_END           = REGION_ENTRIES_OFFSET + REGIONS_MAX * REGION_ENTRY_BYTES;
static constexpr uint32_t REGION_BASE                = 0;
static constexpr uint32_t REGION_LIMIT               = 8;
static constexpr uint32_t REGION_RESERVED            = 16;
static constexpr uint32_t REGION_PERFORMANCE         = 24;
static constexpr uint32_t REGION_COMPRESSED          = 28;
static constexpr uint32_t REGION_SCANOUT             = 29;
static constexpr uint32_t REGION_PROTECTED           = 30;
static constexpr uint32_t REGION_BLACKLIST           = 31;

// The byte `fill_static_info` gives a set flag, nonzero with bit 0 clear, so a flag read from bit 0 alone reads clear
static constexpr uint8_t SET_FLAG_BYTE = 0x80;

static void put_double_word(uint8_t* bytes, uint32_t offset, uint64_t value) {
    string::memcpy(bytes + offset, &value, sizeof(value));
}

static uint8_t* entry_of(queue_test_payload& info, uint32_t index) {
    return info.bytes + REGION_ENTRIES_OFFSET + index * REGION_ENTRY_BYTES;
}

static uint64_t region_base(uint32_t index) {
    return static_cast<uint64_t>(index) << 33 | static_cast<uint64_t>(index) << 20;
}

// Fills `info` with `region_count` regions, region `i` at `i` times 8 GiB plus `i` MiB, `i` times 4 GiB plus 1 MiB
// long, reserved only above bit 31 when odd, and compressed, scanout or protected as `i` modulo 3 is 0, 1 or 2
static void fill_static_info(queue_test_payload& info, uint32_t region_count) {
    string::memset(info.bytes, 0, info.count);
    put_word(info.bytes, REGION_COUNT_OFFSET, region_count);
    // The padding after the count holds no zero, so a count read 64 bits wide fails
    put_word(info.bytes, REGION_PADDING_OFFSET, STALE_WORD);
    for (uint32_t i = 0; i < region_count; i++) {
        uint8_t* entry = entry_of(info, i);
        uint64_t size = static_cast<uint64_t>(i) << 32 | 0x100000;
        put_double_word(entry, REGION_BASE, region_base(i));
        put_double_word(entry, REGION_LIMIT, region_base(i) + size - 1);
        put_double_word(entry, REGION_RESERVED, i % 2 == 1 ? static_cast<uint64_t>(i) << 32 : 0);
        put_word(entry, REGION_PERFORMANCE, 0xFFFFFFFF);
        entry[REGION_COMPRESSED] = i % 3 == 0 ? SET_FLAG_BYTE : 0;
        entry[REGION_SCANOUT] = i % 3 == 1 ? SET_FLAG_BYTE : 0;
        entry[REGION_PROTECTED] = i % 3 == 2 ? SET_FLAG_BYTE : 0;
        string::memset(entry + REGION_BLACKLIST, 0xFF, REGION_ENTRY_BYTES - REGION_BLACKLIST);
    }
}

// The static configuration is asked for with NVIDIA's 2168 zero bytes, and each region the count covers is read in full
// 64 bits with NVIDIA's reserved decision and each flag set from 1 and 0x80, while a valid region past the count is not
TEST(nvidia_message_queues, video_memory_regions_are_read_from_the_static_configuration) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    queue_test_payload info(STATIC_INFO_BYTES);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(info.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    fill_static_info(info, 7);
    put_word(info.bytes, REGION_COUNT_OFFSET, 6);
    // One flag of each kind set to 1, NVIDIA's value for true, so a flag read from bit 7 alone reads clear
    entry_of(info, 3)[REGION_COMPRESSED] = 1;
    entry_of(info, 4)[REGION_SCANOUT] = 1;
    entry_of(info, 5)[REGION_PROTECTED] = 1;
    MOVED_HEADER.write_to(memory);
    store_function_message(memory, message, 0, NVIDIA_GET_GSP_STATIC_INFO, info.bytes, info.count);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 1);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);
    static_configuration configuration = {};
    configuration.regions[6].offset = STALE_WORD;

    EXPECT_EQ(read_static_configuration(conversation, LONG_TIMEOUT_NS, configuration), OK);

    const uint8_t* request = memory.element(0);
    const video_memory_region* regions = configuration.regions;
    uint32_t nonzero_argument_bytes = 0;
    for (uint32_t i = 0; i < STATIC_INFO_BYTES; i++) {
        nonzero_argument_bytes += request[MESSAGE_PAYLOAD + i] != 0 ? 1 : 0;
    }

    EXPECT_EQ(field_at(request, MESSAGE_FUNCTION), NVIDIA_GET_GSP_STATIC_INFO);
    EXPECT_EQ(field_at(request, MESSAGE_RPC_BYTES), RPC_HEADER_BYTES + STATIC_INFO_BYTES);
    EXPECT_EQ(nonzero_argument_bytes, 0u);
    EXPECT_EQ(configuration.region_count, 6u);
    EXPECT_EQ(regions[0].offset, 0u);
    EXPECT_EQ(regions[0].size, 0x100000u);
    EXPECT_EQ(regions[0].reserved_bytes, 0u);
    EXPECT_FALSE(regions[0].is_reserved);
    EXPECT_FALSE(regions[0].is_protected);
    EXPECT_FALSE(regions[0].supports_scanout);
    EXPECT_TRUE(regions[0].supports_compression);
    EXPECT_EQ(regions[1].offset, 0x200100000u);
    EXPECT_EQ(regions[1].size, 0x100100000u);
    EXPECT_EQ(regions[1].reserved_bytes, 0x100000000u);
    EXPECT_TRUE(regions[1].is_reserved);
    EXPECT_FALSE(regions[1].is_protected);
    EXPECT_TRUE(regions[1].supports_scanout);
    EXPECT_FALSE(regions[1].supports_compression);
    EXPECT_EQ(regions[2].offset, 0x400200000u);
    EXPECT_EQ(regions[2].size, 0x200100000u);
    EXPECT_FALSE(regions[2].is_reserved);
    EXPECT_TRUE(regions[2].is_protected);
    EXPECT_FALSE(regions[2].supports_scanout);
    EXPECT_FALSE(regions[2].supports_compression);
    EXPECT_EQ(regions[3].offset, 0x600300000u);
    EXPECT_EQ(regions[3].size, 0x300100000u);
    EXPECT_TRUE(regions[3].is_reserved);
    EXPECT_TRUE(regions[3].supports_compression);
    EXPECT_EQ(regions[4].offset, 0x800400000u);
    EXPECT_EQ(regions[4].size, 0x400100000u);
    EXPECT_FALSE(regions[4].is_reserved);
    EXPECT_TRUE(regions[4].supports_scanout);
    EXPECT_EQ(regions[5].offset, 0xA00500000u);
    EXPECT_EQ(regions[5].size, 0x500100000u);
    EXPECT_TRUE(regions[5].is_reserved);
    EXPECT_TRUE(regions[5].is_protected);
    EXPECT_EQ(regions[6].offset, STALE_WORD);
}

// A reply's payload 8 bytes longer than NVIDIA's static configuration for this firmware
static constexpr uint32_t LONG_STATIC_INFO_BYTES = STATIC_INFO_BYTES + 8;

// A failed result, a payload short of the table's end or a count of 0, 17 or 0x10002 fails with nothing changed, while
// 16 regions with a nonzero private result in a payload ending there and 1 in one longer than NVIDIA's layout pass
TEST(nvidia_message_queues, static_configuration_refused_short_or_miscounted_fails) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    queue_test_payload info(LONG_STATIC_INFO_BYTES);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(info.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    uint32_t last = REGIONS_MAX - 1;
    MOVED_HEADER.write_to(memory);
    fill_static_info(info, REGIONS_MAX);
    store_function_message(memory, message, 0, NVIDIA_GET_GSP_STATIC_INFO, info.bytes, STATIC_INFO_BYTES);
    message.set(MESSAGE_RESULT, RESULT);
    message.seal(MESSAGE_PAYLOAD + STATIC_INFO_BYTES);
    message.store(memory, MOVED_HEADER, 0);
    // A count of 1, so the counted entry ends inside the short payload and only a check of the table's end refuses it
    put_word(info.bytes, REGION_COUNT_OFFSET, 1);
    store_function_message(memory, message, 1, NVIDIA_GET_GSP_STATIC_INFO, info.bytes, REGION_TABLE_END - 1);
    put_word(info.bytes, REGION_COUNT_OFFSET, 0);
    store_function_message(memory, message, 2, NVIDIA_GET_GSP_STATIC_INFO, info.bytes, STATIC_INFO_BYTES);
    put_word(info.bytes, REGION_COUNT_OFFSET, REGIONS_MAX + 1);
    store_function_message(memory, message, 3, NVIDIA_GET_GSP_STATIC_INFO, info.bytes, STATIC_INFO_BYTES);
    put_word(info.bytes, REGION_COUNT_OFFSET, 0x10002);
    store_function_message(memory, message, 4, NVIDIA_GET_GSP_STATIC_INFO, info.bytes, STATIC_INFO_BYTES);
    put_word(info.bytes, REGION_COUNT_OFFSET, REGIONS_MAX);
    store_function_message(
        memory,
        message,
        5,
        NVIDIA_GET_GSP_STATIC_INFO,
        info.bytes,
        REGION_TABLE_END,
        PRIVATE_RESULT
    );
    put_word(info.bytes, REGION_COUNT_OFFSET, 1);
    string::memset(info.bytes + STATIC_INFO_BYTES, STALE_BYTE, LONG_STATIC_INFO_BYTES - STATIC_INFO_BYTES);
    store_function_message(memory, message, 6, NVIDIA_GET_GSP_STATIC_INFO, info.bytes, LONG_STATIC_INFO_BYTES);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 7);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);
    static_configuration configuration = {};
    configuration.regions[0].offset = STALE_WORD;
    configuration.region_count = STALE_WORD;

    EXPECT_EQ(read_static_configuration(conversation, LONG_TIMEOUT_NS, configuration), ERR_FIRMWARE);
    for (uint32_t i = 0; i < 4; i++) {
        EXPECT_EQ(read_static_configuration(conversation, LONG_TIMEOUT_NS, configuration), ERR_BAD_MESSAGE);
    }

    EXPECT_EQ(configuration.region_count, STALE_WORD);
    EXPECT_EQ(configuration.regions[0].offset, STALE_WORD);
    EXPECT_EQ(read_static_configuration(conversation, LONG_TIMEOUT_NS, configuration), OK);
    EXPECT_EQ(configuration.region_count, REGIONS_MAX);
    EXPECT_EQ(configuration.regions[last].offset, region_base(last));
    EXPECT_EQ(configuration.regions[last].size, 0xF00100000u);
    EXPECT_EQ(configuration.regions[last].reserved_bytes, 0xF00000000u);
    EXPECT_TRUE(configuration.regions[last].is_reserved);
    EXPECT_EQ(read_static_configuration(conversation, LONG_TIMEOUT_NS, configuration), OK);
    EXPECT_EQ(configuration.region_count, 1u);
}

// A first region backward only in its bits above 31, the last of 16 a byte backward or one spanning 2^64 bytes fails
// with nothing changed, while a 1-byte region and one from byte 1 to the last pass beside a backward one past the count
TEST(nvidia_message_queues, video_memory_region_ending_below_its_start_or_spanning_all_bytes_fails) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    queue_test_payload info(STATIC_INFO_BYTES);
    status_test_message message(1);
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_NOT_NULL(info.bytes);
    ASSERT_NOT_NULL(message.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    uint32_t last = REGIONS_MAX - 1;
    MOVED_HEADER.write_to(memory);
    fill_static_info(info, REGIONS_MAX);
    put_double_word(entry_of(info, 0), REGION_BASE, 0x200100000);
    put_double_word(entry_of(info, 0), REGION_LIMIT, 0x100200000);
    store_function_message(memory, message, 0, NVIDIA_GET_GSP_STATIC_INFO, info.bytes, info.count);
    fill_static_info(info, REGIONS_MAX);
    put_double_word(entry_of(info, last), REGION_LIMIT, region_base(last) - 1);
    store_function_message(memory, message, 1, NVIDIA_GET_GSP_STATIC_INFO, info.bytes, info.count);
    fill_static_info(info, REGIONS_MAX);
    put_double_word(entry_of(info, 1), REGION_BASE, 0);
    put_double_word(entry_of(info, 1), REGION_LIMIT, 0xFFFFFFFFFFFFFFFF);
    store_function_message(memory, message, 2, NVIDIA_GET_GSP_STATIC_INFO, info.bytes, info.count);
    fill_static_info(info, 3);
    put_word(info.bytes, REGION_COUNT_OFFSET, 2);
    put_double_word(entry_of(info, 0), REGION_LIMIT, 0);
    put_double_word(entry_of(info, 1), REGION_BASE, 1);
    put_double_word(entry_of(info, 1), REGION_LIMIT, 0xFFFFFFFFFFFFFFFF);
    put_double_word(entry_of(info, 2), REGION_LIMIT, region_base(2) - 1);
    store_function_message(memory, message, 3, NVIDIA_GET_GSP_STATIC_INFO, info.bytes, info.count);
    memory.write(STATUS_QUEUE_OFFSET + HEADER_WRITE_POINTER, 4);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);
    static_configuration configuration = {};
    configuration.regions[0].offset = STALE_WORD;
    configuration.regions[2].offset = STALE_WORD;
    configuration.region_count = STALE_WORD;

    for (uint32_t i = 0; i < 3; i++) {
        EXPECT_EQ(read_static_configuration(conversation, LONG_TIMEOUT_NS, configuration), ERR_BAD_MESSAGE);
    }

    EXPECT_EQ(configuration.region_count, STALE_WORD);
    EXPECT_EQ(configuration.regions[0].offset, STALE_WORD);
    EXPECT_EQ(read_static_configuration(conversation, LONG_TIMEOUT_NS, configuration), OK);
    EXPECT_EQ(configuration.region_count, 2u);
    EXPECT_EQ(configuration.regions[0].offset, 0u);
    EXPECT_EQ(configuration.regions[0].size, 1u);
    EXPECT_EQ(configuration.regions[1].offset, 1u);
    EXPECT_EQ(configuration.regions[1].size, 0xFFFFFFFFFFFFFFFFu);
    EXPECT_EQ(configuration.regions[2].offset, STALE_WORD);
}

// Both requests wait for their replies only as long as their callers give, each failing with the wait's timeout on a
// queue that stays empty, the configuration left unchanged
TEST(nvidia_message_queues, message_version_and_static_configuration_wait_out_their_timeouts) {
    fake_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    request_test_bar0 bar0;
    gsp conversation;
    ASSERT_NOT_NULL(bar0.bytes);
    ASSERT_EQ(start_conversation(conversation, memory, bar0), OK);
    MOVED_HEADER.write_to(memory);
    ASSERT_EQ(conversation.link_status_queue(SHORT_TIMEOUT_NS), OK);
    static_configuration configuration = {};
    configuration.regions[0].offset = STALE_WORD;
    configuration.region_count = STALE_WORD;

    uint64_t start = clock::now_ns();
    EXPECT_EQ(confirm_message_version(conversation, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t version_elapsed = clock::now_ns() - start;
    start = clock::now_ns();
    EXPECT_EQ(read_static_configuration(conversation, SHORT_TIMEOUT_NS, configuration), ERR_TIMEOUT);
    uint64_t configuration_elapsed = clock::now_ns() - start;

    EXPECT_GE(version_elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(version_elapsed, WAIT_TIME_LIMIT_NS);
    EXPECT_GE(configuration_elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(configuration_elapsed, WAIT_TIME_LIMIT_NS);
    EXPECT_EQ(configuration.region_count, STALE_WORD);
    EXPECT_EQ(configuration.regions[0].offset, STALE_WORD);
}
