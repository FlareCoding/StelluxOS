// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/boot_messages.h"
#include "drivers/graphics/nvidia/gsp.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_boot_messages);

using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::OK;
using drivers::nvidia::QUEUE_MEMORY_BYTES;
using drivers::nvidia::boot_console_bytes;
using drivers::nvidia::boot_framebuffer;
using drivers::nvidia::card_placement;
using drivers::nvidia::cpu_sequencer_context;
using drivers::nvidia::gsp;
using drivers::nvidia::queue_boot_messages;
using drivers::nvidia::registers;

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
static constexpr uint32_t SYSTEM_CONSOLE_BYTES             = 48;
static constexpr uint32_t SYSTEM_USER_ADDRESS_END          = 56;
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
    .registers_address = 0x1234567000000,
    .bar1_address      = 0x2345678000000,
    .bar3_address      = 0x3456789000000,
    .bus               = 0xFF,
    .device            = 0x1F,
};

// NVIDIA's encoding of that bus and device
static constexpr uint64_t CARD_PCI_ADDRESS = 0xFF1F;

// A console reservation above 4 GiB, so a field cut to 32 bits shows
static constexpr uint64_t CONSOLE_BYTES = 0x123450000;

// The offset in BAR0 of its window into video memory, which NVIDIA's driver sends in place of instance memory it lacks
static constexpr uint64_t BAR0_VIDEO_MEMORY_WINDOW_OFFSET = 0x700000;

// Where the VBIOS sets a boot console up in NVIDIA's instance memory
static constexpr uint64_t VBIOS_CONSOLE_OFFSET = 0x1000000;

// What NVIDIA's driver keeps when it finds no console on the card or one whose height times its pitch is 0, since it
// learns whether the card booted through UEFI only from the firmware
static constexpr uint64_t NON_UEFI_CONSOLE_BYTES = 0x40000;

// Whether the chip's fuses leave its display enabled
static constexpr bool WITH_DISPLAY    = true;
static constexpr bool WITHOUT_DISPLAY = false;

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

// Starts `conversation` on the fake memory with a BAR0 view at address 0 and an empty sequencer context, since queueing
// the boot messages reaches neither
static int32_t start_conversation(gsp& conversation, boot_message_queue_memory& memory) {
    registers no_registers(0);
    cpu_sequencer_context unused_sequencer_context = {};

    return conversation.start(no_registers, memory.bytes, 0, unused_sequencer_context);
}

// Counts the nonzero bytes of the system information in the first element outside the fields this driver writes
static uint32_t nonzero_unwritten_system_bytes(const boot_message_queue_memory& memory) {
    const uint8_t* system = memory.bytes + memory.payload_offset(0);
    uint32_t nonzero_bytes = 0;
    for (uint32_t offset = 0; offset < SYSTEM_INFO_BYTES; offset++) {
        bool is_written_field =
            offset < SYSTEM_SIMULATION_BUFFER_ADDRESS
            || (offset >= SYSTEM_CONSOLE_BYTES && offset < SYSTEM_USER_ADDRESS_END)
            || (offset >= SYSTEM_PCI_CONFIG_MIRROR_OFFSET && offset <= SYSTEM_CPU_ARCHITECTURE)
            || (offset >= SYSTEM_HYPERVISOR && offset < SYSTEM_IS_PASSTHROUGH);
        nonzero_bytes += !is_written_field && system[offset] != 0 ? 1 : 0;
    }

    return nonzero_bytes;
}

// The system information carries the card's BARs, bus and device, the console reservation, the configuration mirror,
// the architecture and an unknown hypervisor with zeros elsewhere, and an empty registry follows, each in an element
TEST(nvidia_boot_messages, system_information_then_empty_registry_queued) {
    boot_message_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    gsp conversation;
    ASSERT_EQ(start_conversation(conversation, memory), OK);

    EXPECT_EQ(queue_boot_messages(conversation, CARD, CONSOLE_BYTES), OK);

    uint32_t system_offset = memory.payload_offset(0);
    uint32_t registry_offset = memory.payload_offset(1);
    EXPECT_EQ(memory.read(WRITE_POINTER), 2u);
    EXPECT_EQ(memory.read(system_offset - MESSAGE_PAYLOAD + MESSAGE_FUNCTION), SET_SYSTEM_INFO);
    EXPECT_EQ(memory.read(system_offset - MESSAGE_PAYLOAD + MESSAGE_RPC_BYTES), RPC_HEADER_BYTES + SYSTEM_INFO_BYTES);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_REGISTERS_ADDRESS), CARD.registers_address);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_VIDEO_MEMORY_ADDRESS), CARD.bar1_address);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_INSTANCE_MEMORY_ADDRESS), CARD.bar3_address);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_PCI_ADDRESS), CARD_PCI_ADDRESS);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_CONSOLE_BYTES), CONSOLE_BYTES);
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
    gsp conversation;
    ASSERT_EQ(start_conversation(conversation, memory), OK);

    memory.write(FIRMWARE_READ_POINTER, 2);
    EXPECT_EQ(queue_boot_messages(conversation, CARD, CONSOLE_BYTES), ERR_QUEUE_FULL);
    EXPECT_EQ(memory.read(WRITE_POINTER), 1u);
    EXPECT_EQ(memory.read(memory.payload_offset(0) - MESSAGE_PAYLOAD + MESSAGE_FUNCTION), SET_SYSTEM_INFO);

    EXPECT_EQ(queue_boot_messages(conversation, CARD, CONSOLE_BYTES), ERR_QUEUE_FULL);
    EXPECT_EQ(memory.read(WRITE_POINTER), 1u);
}

// NVIDIA's driver numbers only the assigned BARs that hold memory, so without BAR1 its framebuffer BAR is BAR3, and
// instance memory it lacks goes out as BAR0's window into video memory
TEST(nvidia_boot_messages, unassigned_bars_go_out_as_nvidia_sends_them) {
    boot_message_queue_memory memory;
    ASSERT_NOT_NULL(memory.bytes);
    uint32_t system_offset = memory.payload_offset(0);
    uint64_t window_address = CARD.registers_address + BAR0_VIDEO_MEMORY_WINDOW_OFFSET;

    card_placement card = CARD;
    card.bar1_address = 0;
    gsp conversation_without_bar1;
    ASSERT_EQ(start_conversation(conversation_without_bar1, memory), OK);
    EXPECT_EQ(queue_boot_messages(conversation_without_bar1, card, 0), OK);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_VIDEO_MEMORY_ADDRESS), CARD.bar3_address);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_INSTANCE_MEMORY_ADDRESS), window_address);

    card = CARD;
    card.bar3_address = 0;
    gsp conversation_without_bar3;
    ASSERT_EQ(start_conversation(conversation_without_bar3, memory), OK);
    EXPECT_EQ(queue_boot_messages(conversation_without_bar3, card, 0), OK);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_VIDEO_MEMORY_ADDRESS), CARD.bar1_address);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_INSTANCE_MEMORY_ADDRESS), window_address);

    card.bar1_address = 0;
    gsp conversation_without_either;
    ASSERT_EQ(start_conversation(conversation_without_either, memory), OK);
    EXPECT_EQ(queue_boot_messages(conversation_without_either, card, 0), OK);
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_VIDEO_MEMORY_ADDRESS), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.read_wide(system_offset + SYSTEM_INSTANCE_MEMORY_ADDRESS), window_address);
}

// A framebuffer at the start of BAR1 takes its height times its pitch rounded up to 64 KiB, and one whose product is
// already a multiple of 64 KiB takes the product alone
TEST(nvidia_boot_messages, console_at_start_of_bar1_takes_its_rows_rounded_up) {
    boot_framebuffer framebuffer = {.address = CARD.bar1_address, .height = 1080, .pitch = 7680};
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITH_DISPLAY), static_cast<uint64_t>(0x7F0000));

    framebuffer = {.address = CARD.bar1_address, .height = 1440, .pitch = 16384};
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITH_DISPLAY), static_cast<uint64_t>(0x1680000));
}

// This driver's own rule: a height and a pitch past 16 bits count in full, where NVIDIA's driver keeps 16 bits of each
TEST(nvidia_boot_messages, console_past_16_bit_dimensions_takes_all_its_rows) {
    boot_framebuffer framebuffer = {.address = CARD.bar1_address, .height = 0x10001, .pitch = 0x10000};
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITH_DISPLAY), static_cast<uint64_t>(0x100010000));
}

// A framebuffer 16 MiB into BAR3, NVIDIA's instance memory, where the VBIOS sets the console up, takes its height times
// its pitch rounded up to 64 KiB
TEST(nvidia_boot_messages, console_16_mib_into_bar3_takes_its_rows_rounded_up) {
    boot_framebuffer framebuffer = {
        .address = CARD.bar3_address + VBIOS_CONSOLE_OFFSET,
        .height = 600,
        .pitch = 3200,
    };

    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITH_DISPLAY), static_cast<uint64_t>(0x1E0000));
}

// Without BAR1, NVIDIA's framebuffer BAR is BAR3 and it has no instance memory, so a framebuffer at BAR3's start takes
// its height times its pitch rounded up to 64 KiB, and one 16 MiB into BAR3 takes 256 KiB
TEST(nvidia_boot_messages, console_without_bar1_is_looked_for_at_bar3_start) {
    card_placement card = CARD;
    card.bar1_address = 0;

    boot_framebuffer framebuffer = {.address = CARD.bar3_address, .height = 1080, .pitch = 7680};
    EXPECT_EQ(boot_console_bytes(card, framebuffer, WITH_DISPLAY), static_cast<uint64_t>(0x7F0000));

    framebuffer.address = CARD.bar3_address + VBIOS_CONSOLE_OFFSET;
    EXPECT_EQ(boot_console_bytes(card, framebuffer, WITH_DISPLAY), NON_UEFI_CONSOLE_BYTES);
}

// A framebuffer anywhere else in the card's BARs, outside them or missing takes the 256 KiB NVIDIA's driver keeps
// before it learns whether the card booted through UEFI
TEST(nvidia_boot_messages, console_elsewhere_takes_256_kib) {
    boot_framebuffer framebuffer = {.address = CARD.bar1_address + 0x1000, .height = 1080, .pitch = 7680};
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITH_DISPLAY), NON_UEFI_CONSOLE_BYTES);

    framebuffer.address = CARD.bar3_address;
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITH_DISPLAY), NON_UEFI_CONSOLE_BYTES);

    framebuffer.address = CARD.bar3_address + VBIOS_CONSOLE_OFFSET + 0x1000;
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITH_DISPLAY), NON_UEFI_CONSOLE_BYTES);

    framebuffer.address = 0x80000000;
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITH_DISPLAY), NON_UEFI_CONSOLE_BYTES);

    framebuffer.address = 0;
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITH_DISPLAY), NON_UEFI_CONSOLE_BYTES);
}

// This driver's own rule: an unassigned BAR holds no console, so a framebuffer at physical 0 or 16 MiB, which NVIDIA's
// driver checks in place of a BAR it lacks, takes 256 KiB rather than its height times its pitch
TEST(nvidia_boot_messages, console_at_unassigned_bar_place_takes_256_kib) {
    card_placement card = CARD;
    card.bar3_address = 0;
    boot_framebuffer framebuffer = {.address = VBIOS_CONSOLE_OFFSET, .height = 1080, .pitch = 7680};
    EXPECT_EQ(boot_console_bytes(card, framebuffer, WITH_DISPLAY), NON_UEFI_CONSOLE_BYTES);

    card = CARD;
    card.bar1_address = 0;
    EXPECT_EQ(boot_console_bytes(card, framebuffer, WITH_DISPLAY), NON_UEFI_CONSOLE_BYTES);

    card.bar3_address = 0;
    framebuffer.address = 0;
    EXPECT_EQ(boot_console_bytes(card, framebuffer, WITH_DISPLAY), NON_UEFI_CONSOLE_BYTES);
}

// A framebuffer at the start of BAR1 whose height times its pitch is 0 takes the same 256 KiB, as when NVIDIA's driver
// finds no console
TEST(nvidia_boot_messages, console_of_no_bytes_takes_256_kib) {
    boot_framebuffer framebuffer = {.address = CARD.bar1_address, .height = 0, .pitch = 7680};
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITH_DISPLAY), NON_UEFI_CONSOLE_BYTES);

    framebuffer = {.address = CARD.bar1_address, .height = 1080, .pitch = 0};
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITH_DISPLAY), NON_UEFI_CONSOLE_BYTES);
}

// A chip whose fuses disable its display takes no bytes, with a console at BAR1's start or none at all
TEST(nvidia_boot_messages, chip_without_display_takes_no_bytes) {
    boot_framebuffer framebuffer = {.address = CARD.bar1_address, .height = 1080, .pitch = 7680};
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITHOUT_DISPLAY), static_cast<uint64_t>(0));

    framebuffer.address = 0;
    EXPECT_EQ(boot_console_bytes(CARD, framebuffer, WITHOUT_DISPLAY), static_cast<uint64_t>(0));
}
