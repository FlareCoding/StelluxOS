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

namespace {

// The system information in NVIDIA's layout, padding written out. `host_fields` holds a flag NVIDIA's driver never
// sets and what it probes of the host's chipset, the PCIe link above the card and the host's ACPI methods for the GPU
struct system_information {
    uint64_t registers_address;
    uint64_t video_memory_address;
    uint64_t instance_memory_address;
    uint64_t pci_address;                  // the domain in bits 63:32, the bus in bits 15:8 and the device in 7:0
    uint64_t simulation_buffer_address;    // a buffer only NVIDIA's simulated GPUs use
    uint64_t pcie_atomic_operations;       // the PCIe atomic operations enabled for the card
    uint64_t console_bytes;                // the boot console's video memory NVIDIA's driver reserves
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

int32_t queue_boot_messages(message_queues& queues, const card_placement& card) {
    system_information system = {};
    system.registers_address = card.registers_address;
    system.video_memory_address = card.video_memory_address;
    system.instance_memory_address = card.instance_memory_address;
    system.pci_address = static_cast<uint64_t>(card.bus) << 8 | card.device;
    system.pci_config_mirror_offset = PCI_CONFIG_MIRROR_OFFSET;
    system.pci_config_mirror_bytes = PCI_CONFIG_MIRROR_BYTES;
    system.cpu_architecture = CPU_ARCHITECTURE;
    system.hypervisor = HYPERVISOR_UNKNOWN;

    int32_t rc = queues.send(FUNCTION_SET_SYSTEM_INFORMATION, &system, sizeof(system));
    if (rc != OK) {
        return rc;
    }

    registry_table registry = {};
    registry.bytes = sizeof(registry);

    return queues.send(FUNCTION_SET_REGISTRY, &registry, sizeof(registry));
}

} // namespace drivers::nvidia
