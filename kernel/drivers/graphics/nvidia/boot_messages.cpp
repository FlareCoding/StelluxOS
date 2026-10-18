// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/boot_messages.h"
#include "drivers/graphics/nvidia/nvidia.h"

namespace drivers::nvidia {

// NVIDIA's numbers for the two messages
constexpr uint32_t FUNCTION_SET_SYSTEM_INFORMATION = 72;
constexpr uint32_t FUNCTION_SET_REGISTRY           = 73;

// Where BAR0 mirrors the card's PCI configuration space, the same on every chip in the chip table
constexpr uint32_t PCI_CONFIG_MIRROR_OFFSET = 0x88000;
constexpr uint32_t PCI_CONFIG_MIRROR_BYTES  = 0x1000;

// NVIDIA's code for the CPU architecture its driver is built for
#if defined(__x86_64__)
constexpr uint8_t CPU_ARCHITECTURE = 0;
#elif defined(__aarch64__)
constexpr uint8_t CPU_ARCHITECTURE = 3;
#else
#error "NVIDIA's CPU architecture code is restated for x86_64 and aarch64 only"
#endif

// NVIDIA's hypervisor type when it finds none, as on a machine without virtualization
constexpr uint32_t HYPERVISOR_UNKNOWN = 4;

// The offset in BAR0 of its window into video memory, which NVIDIA's driver sends in place of instance memory it lacks
constexpr uint64_t BAR0_VIDEO_MEMORY_WINDOW_OFFSET = 0x700000;

// Where the VBIOS sets a boot console up in NVIDIA's instance memory
constexpr uint64_t VBIOS_CONSOLE_OFFSET = 0x1000000;

// The unit to which NVIDIA's driver rounds up the bytes it keeps for a boot console
constexpr uint64_t CONSOLE_RESERVATION_UNIT = 0x10000;

// What NVIDIA's driver keeps when it finds no console on the card or one whose height times its pitch is 0, since it
// learns whether the card booted through UEFI only from the firmware
constexpr uint64_t NON_UEFI_CONSOLE_BYTES = 0x40000;

namespace {

// The physical addresses of NVIDIA's framebuffer BAR and instance memory, each 0 when NVIDIA's driver lacks it
struct nvidia_bars {
    uint64_t framebuffer_address;
    uint64_t instance_memory_address;
};

// The system information in NVIDIA's layout, padding written out. `host_fields` holds a flag NVIDIA's driver never
// sets and what it probes of the host's chipset, the PCIe link above the card and the host's ACPI methods for the GPU
struct system_information {
    uint64_t registers_address;
    uint64_t video_memory_address;
    uint64_t instance_memory_address;
    uint64_t pci_address;                  // the domain in bits 63:32, the bus in bits 15:8 and the device in 7:0
    uint64_t simulation_buffer_address;    // a buffer only NVIDIA's simulated GPUs use
    uint64_t pcie_atomic_operations;       // the PCIe atomic operations enabled for the card
    uint64_t console_bytes;                // video memory the firmware leaves alone for a boot console
    uint64_t user_address_end;             // where a user process's addresses end on the CPU
    uint32_t pci_config_mirror_offset;
    uint32_t pci_config_mirror_bytes;
    uint8_t  cpu_architecture;
    uint8_t  padding_after_architecture[7];
    uint8_t  host_fields[524];
    uint32_t hypervisor;
    uint8_t  is_passthrough;               // whether a hypervisor passes the card through to this machine
    uint8_t  padding_after_passthrough[7];
    uint64_t timer_offset_ns;              // between the CPU's clock and the card's
    uint64_t virtual_function_info[5];     // the PCI functions the card can give virtual machines (SR-IOV): their
                                           // count, the first one's offset, its BAR addresses and which are 64-bit
    uint8_t  supports_reset_event;         // whether the driver handles an event for a reset of the firmware
    uint8_t  padding_at_end[7];
};

// NVIDIA's registry table, its entries and the names and data they point to following these fields
struct registry_table {
    uint32_t bytes;       // of the whole table, its entries and the names and data they point to included
    uint32_t entry_count;
};

} // namespace

static_assert(sizeof(system_information) == 672, "the system information takes NVIDIA's 672 bytes");
static_assert(__builtin_offsetof(system_information, pci_config_mirror_offset) == 64);
static_assert(__builtin_offsetof(system_information, host_fields) == 80);
static_assert(__builtin_offsetof(system_information, hypervisor) == 604);
static_assert(__builtin_offsetof(system_information, timer_offset_ns) == 616);
static_assert(sizeof(registry_table) == 8, "an empty registry table takes NVIDIA's 8 bytes");

// NVIDIA's driver numbers only the assigned BARs that hold memory, so without BAR1 it takes BAR3 as its framebuffer BAR
// and has no instance memory
static nvidia_bars number_bars_as_nvidia(const card_placement& card) {
    if (card.bar1_address == 0) {
        return {.framebuffer_address = card.bar3_address, .instance_memory_address = 0};
    }

    return {.framebuffer_address = card.bar1_address, .instance_memory_address = card.bar3_address};
}

uint64_t boot_console_bytes(const card_placement& card, const boot_framebuffer& framebuffer, bool has_display) {
    if (!has_display) {
        return 0;
    }

    nvidia_bars bars = number_bars_as_nvidia(card);
    bool is_at_framebuffer_bar_start =
        bars.framebuffer_address != 0
        && framebuffer.address == bars.framebuffer_address;
    bool is_at_vbios_console_offset =
        bars.instance_memory_address != 0
        && framebuffer.address == bars.instance_memory_address + VBIOS_CONSOLE_OFFSET;
    uint64_t bytes = 0;
    if (is_at_framebuffer_bar_start || is_at_vbios_console_offset) {
        bytes = framebuffer.height * framebuffer.pitch;
    }

    if (bytes == 0) {
        bytes = NON_UEFI_CONSOLE_BYTES;
    }

    return (bytes + CONSOLE_RESERVATION_UNIT - 1) & ~(CONSOLE_RESERVATION_UNIT - 1);
}

int32_t queue_boot_messages(gsp& conversation, const card_placement& card, uint64_t console_bytes) {
    nvidia_bars bars = number_bars_as_nvidia(card);
    uint64_t instance_memory_address = bars.instance_memory_address;
    if (instance_memory_address == 0) {
        instance_memory_address = card.registers_address + BAR0_VIDEO_MEMORY_WINDOW_OFFSET;
    }

    system_information system = {};
    system.registers_address = card.registers_address;
    system.video_memory_address = bars.framebuffer_address;
    system.instance_memory_address = instance_memory_address;
    system.pci_address = static_cast<uint64_t>(card.bus) << 8 | card.device;
    system.console_bytes = console_bytes;
    system.pci_config_mirror_offset = PCI_CONFIG_MIRROR_OFFSET;
    system.pci_config_mirror_bytes = PCI_CONFIG_MIRROR_BYTES;
    system.cpu_architecture = CPU_ARCHITECTURE;
    system.hypervisor = HYPERVISOR_UNKNOWN;

    int32_t rc = conversation.send(FUNCTION_SET_SYSTEM_INFORMATION, &system, sizeof(system));
    if (rc != OK) {
        return rc;
    }

    registry_table registry = {};
    registry.bytes = sizeof(registry);

    return conversation.send(FUNCTION_SET_REGISTRY, &registry, sizeof(registry));
}

} // namespace drivers::nvidia
