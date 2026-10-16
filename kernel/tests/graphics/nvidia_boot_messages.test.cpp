// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/boot_messages.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_boot_messages);

using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::OK;
using drivers::nvidia::QUEUE_MEMORY_BYTES;
using drivers::nvidia::card_placement;
using drivers::nvidia::message_queues;
using drivers::nvidia::queue_boot_messages;

// Where the command queue's elements, its write pointer and the firmware's read pointer of it lie in the queue
// memory, and a message's fields as offsets from its first byte, as NVIDIA's driver lays them out
static constexpr uint32_t FIRST_ELEMENT         = 0x2000;
static constexpr uint32_t ELEMENT_BYTES         = 0x1000;
static constexpr uint32_t WRITE_POINTER         = 0x1010;
static constexpr uint32_t FIRMWARE_READ_POINTER = 0x1020;
static constexpr uint32_t MESSAGE_RPC_BYTES     = 56;
static constexpr uint32_t MESSAGE_FUNCTION      = 60;
static constexpr uint32_t MESSAGE_PAYLOAD       = 80;
static constexpr uint32_t RPC_HEADER_BYTES      = 32;

// NVIDIA's numbers for the two messages and their payloads' sizes
static constexpr uint32_t SET_SYSTEM_INFO   = 72;
static constexpr uint32_t SET_REGISTRY      = 73;
static constexpr uint32_t SYSTEM_INFO_BYTES = 672;
static constexpr uint32_t REGISTRY_BYTES    = 8;

// The system information's fields this driver writes and the fields following each run of them, as offsets from its
// first byte
static constexpr uint32_t SYSTEM_REGISTERS_ADDRESS         = 0;
static constexpr uint32_t SYSTEM_VIDEO_MEMORY_ADDRESS      = 8;
static constexpr uint32_t SYSTEM_INSTANCE_MEMORY_ADDRESS   = 16;
static constexpr uint32_t SYSTEM_PCI_ADDRESS               = 24;
static constexpr uint32_t SYSTEM_SIMULATION_BUFFER_ADDRESS = 32;
static constexpr uint32_t SYSTEM_PCI_CONFIG_MIRROR_OFFSET  = 64;
static constexpr uint32_t SYSTEM_PCI_CONFIG_MIRROR_BYTES   = 68;
static constexpr uint32_t SYSTEM_CPU_ARCHITECTURE          = 72;
static constexpr uint32_t SYSTEM_HYPERVISOR                = 604;
static constexpr uint32_t SYSTEM_IS_PASSTHROUGH            = 608;

// The registry table's entry count, as an offset from its first byte
static constexpr uint32_t REGISTRY_ENTRY_COUNT = 4;

// What the driver writes in the system information's fields not taken from the card, NVIDIA's values on a host where
// it finds no hypervisor
static constexpr uint32_t PCI_CONFIG_MIRROR_OFFSET = 0x88000;
static constexpr uint32_t PCI_CONFIG_MIRROR_BYTES  = 0x1000;
static constexpr uint32_t HYPERVISOR_UNKNOWN       = 4;
#if defined(__x86_64__)
static constexpr uint8_t CPU_ARCHITECTURE = 0;
#elif defined(__aarch64__)
static constexpr uint8_t CPU_ARCHITECTURE = 3;
#endif

// Addresses above 4 GiB and a bus and device that fill their bits, so a field cut short or placed wrong shows
static constexpr card_placement CARD = {
    .registers_address       = 0x1234567000000,
    .video_memory_address    = 0x2345678000000,
    .instance_memory_address = 0x3456789000000,
    .bus                     = 0xFF,
    .device                  = 0x1F,
};

// NVIDIA's encoding of that bus and device
static constexpr uint64_t CARD_PCI_ADDRESS = 0xFF1F;

// A fake of the queue memory the queues zero when they lay it out
struct boot_message_queue_memory {
    uint8_t* bytes = static_cast<uint8_t*>(heap::ualloc(QUEUE_MEMORY_BYTES));

    ~boot_message_queue_memory() { heap::ufree(bytes); }

    uint32_t read(uint32_t offset) const { return *reinterpret_cast<uint32_t*>(bytes + offset); }
    uint64_t read_wide(uint32_t offset) const { return *reinterpret_cast<uint64_t*>(bytes + offset); }
    void write(uint32_t offset, uint32_t value) { *reinterpret_cast<uint32_t*>(bytes + offset) = value; }

    uint32_t payload_offset(uint32_t element) const {
        return FIRST_ELEMENT + element * ELEMENT_BYTES + MESSAGE_PAYLOAD;
    }
};

// Counts the nonzero bytes of the system information in the first element outside the fields this driver writes
static uint32_t nonzero_unwritten_system_bytes(const boot_message_queue_memory& memory) {
    const uint8_t* system = memory.bytes + memory.payload_offset(0);
    uint32_t nonzero_bytes = 0;
    for (uint32_t offset = 0; offset < SYSTEM_INFO_BYTES; offset++) {
        bool is_written_field =
            offset < SYSTEM_SIMULATION_BUFFER_ADDRESS
            || (offset >= SYSTEM_PCI_CONFIG_MIRROR_OFFSET && offset <= SYSTEM_CPU_ARCHITECTURE)
            || (offset >= SYSTEM_HYPERVISOR && offset < SYSTEM_IS_PASSTHROUGH);
        nonzero_bytes += !is_written_field && system[offset] != 0 ? 1 : 0;
    }

    return nonzero_bytes;
}

// The system information comes first and carries the card's BARs, its bus and device, the configuration mirror, the
// CPU's architecture and an unknown hypervisor with zeros elsewhere, then an empty registry, each in an element
TEST(nvidia_boot_messages, system_information_then_empty_registry_queued) {
    boot_message_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, 0);

    EXPECT_EQ(queue_boot_messages(queues, CARD), OK);

    uint32_t system_offset = memory.payload_offset(0);
    uint32_t registry_offset = memory.payload_offset(1);
    EXPECT_EQ(memory.read(WRITE_POINTER), 2u);
    EXPECT_EQ(memory.read(system_offset - MESSAGE_PAYLOAD + MESSAGE_FUNCTION), SET_SYSTEM_INFO);
    EXPECT_EQ(memory.read(system_offset - MESSAGE_PAYLOAD + MESSAGE_RPC_BYTES), RPC_HEADER_BYTES + SYSTEM_INFO_BYTES);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_REGISTERS_ADDRESS), CARD.registers_address);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_VIDEO_MEMORY_ADDRESS), CARD.video_memory_address);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_INSTANCE_MEMORY_ADDRESS), CARD.instance_memory_address);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_PCI_ADDRESS), CARD_PCI_ADDRESS);
    EXPECT_EQ(memory.read(system_offset + SYSTEM_PCI_CONFIG_MIRROR_OFFSET), PCI_CONFIG_MIRROR_OFFSET);
    EXPECT_EQ(memory.read(system_offset + SYSTEM_PCI_CONFIG_MIRROR_BYTES), PCI_CONFIG_MIRROR_BYTES);
    EXPECT_EQ(memory.bytes[system_offset + SYSTEM_CPU_ARCHITECTURE], CPU_ARCHITECTURE);
    EXPECT_EQ(memory.read(system_offset + SYSTEM_HYPERVISOR), HYPERVISOR_UNKNOWN);
    EXPECT_EQ(nonzero_unwritten_system_bytes(memory), 0u);
    EXPECT_EQ(memory.read(registry_offset - MESSAGE_PAYLOAD + MESSAGE_FUNCTION), SET_REGISTRY);
    EXPECT_EQ(memory.read(registry_offset - MESSAGE_PAYLOAD + MESSAGE_RPC_BYTES), RPC_HEADER_BYTES + REGISTRY_BYTES);
    EXPECT_EQ(memory.read(registry_offset), REGISTRY_BYTES);
    EXPECT_EQ(memory.read(registry_offset + REGISTRY_ENTRY_COUNT), 0u);
}

// With room for one message, the system information stays queued and the registry's error returns, and with room for
// none, nothing is queued
TEST(nvidia_boot_messages, full_queue_returns_its_error) {
    boot_message_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    message_queues queues(memory.bytes, 0);

    memory.write(FIRMWARE_READ_POINTER, 2);
    EXPECT_EQ(queue_boot_messages(queues, CARD), ERR_QUEUE_FULL);
    EXPECT_EQ(memory.read(WRITE_POINTER), 1u);
    EXPECT_EQ(memory.read(memory.payload_offset(0) - MESSAGE_PAYLOAD + MESSAGE_FUNCTION), SET_SYSTEM_INFO);

    EXPECT_EQ(queue_boot_messages(queues, CARD), ERR_QUEUE_FULL);
    EXPECT_EQ(memory.read(WRITE_POINTER), 1u);
}
