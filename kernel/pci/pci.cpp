#include "pci/pci.h"
#include "pci/pci_class_codes.h"
#include "acpi/acpi.h"
#include "fdt/fdt.h"
#include "mm/vmm.h"
#include "hw/mmio.h"
#include "common/logging.h"
#include "common/string.h"

namespace pci {

constexpr uint16_t VENDOR_INVALID   = 0xFFFF;
constexpr uint32_t BAR_PROBE_VALUE  = 0xFFFFFFFF;
constexpr uint8_t  SLOTS_PER_BUS    = 32;
constexpr uint8_t  FUNCS_PER_DEVICE = 8;
constexpr uint8_t  CAP_PTR_MASK     = 0xFC;
constexpr uint8_t  CAP_MIN_OFFSET   = 0x40;
constexpr uint32_t MAX_CAP_WALK     = 48;

// Bridge config offsets (Type 1 header)
constexpr uint16_t CFG_SECONDARY_BUS = 0x19;

// Broadcom BCM2711 PCIe controller registers
constexpr uint16_t BRCM_EXT_CFG_INDEX = 0x9000;
constexpr uint16_t BRCM_EXT_CFG_DATA  = 0x8000;
constexpr uint16_t BRCM_PCIE_STATUS   = 0x4068;
constexpr uint32_t BRCM_PHYLINKUP     = 0x10;
constexpr uint32_t BRCM_DL_ACTIVE     = 0x20;

// MCFG table structs (packed, spec-facing)
struct mcfg_entry {
    uint64_t base_address;
    uint16_t segment_group;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} __attribute__((packed));
static_assert(sizeof(mcfg_entry) == 16);

struct mcfg_table {
    acpi::sdt_header header;
    uint64_t reserved;
} __attribute__((packed));
static_assert(sizeof(mcfg_table) == 44);

// Config access backend
enum class config_backend : uint8_t { ECAM, BROADCOM };

// Global state
__PRIVILEGED_DATA static bool g_initialized = false;
__PRIVILEGED_DATA static device g_devices[MAX_DEVICES] = {};
__PRIVILEGED_DATA static uint32_t g_device_count = 0;
__PRIVILEGED_DATA static config_backend g_backend = config_backend::ECAM;
__PRIVILEGED_DATA static uintptr_t g_ecam_va = 0;
__PRIVILEGED_DATA static uint8_t g_ecam_start_bus = 0;
__PRIVILEGED_DATA static uint8_t g_ecam_end_bus = 0;
__PRIVILEGED_DATA static uint8_t g_start_bus = 0;
__PRIVILEGED_DATA static uintptr_t g_brcm_base = 0;

static const bar g_null_bar = {0, 0, BAR_NONE, false};

static inline uintptr_t bdf_to_ecam_offset(uint8_t bus, uint8_t slot, uint8_t func) {
    return (static_cast<uintptr_t>(bus) << 20)
         + (static_cast<uintptr_t>(slot) << 15)
         + (static_cast<uintptr_t>(func) << 12);
}

static inline uintptr_t dev_base_for(uint8_t bus, uint8_t slot, uint8_t func) {
    if (g_backend == config_backend::ECAM) {
        return g_ecam_va + bdf_to_ecam_offset(bus - g_ecam_start_bus, slot, func);
    }
    // Broadcom: root complex at 0:0.0 uses sentinel 0; downstream uses INDEX value
    if (bus == 0 && slot == 0 && func == 0) return 0;
    return bdf_to_ecam_offset(bus, slot, func);
}


__PRIVILEGED_CODE static uint8_t cfg_read8(uintptr_t base, uint16_t offset) {
    if (g_backend == config_backend::ECAM) {
        return mmio::read8(base + offset);
    }
    if (base == 0) return mmio::read8(g_brcm_base + offset);
    mmio::write32(g_brcm_base + BRCM_EXT_CFG_INDEX, static_cast<uint32_t>(base));
    return mmio::read8(g_brcm_base + BRCM_EXT_CFG_DATA + offset);
}

__PRIVILEGED_CODE static uint16_t cfg_read16(uintptr_t base, uint16_t offset) {
    if (g_backend == config_backend::ECAM) {
        return mmio::read16(base + offset);
    }
    if (base == 0) return mmio::read16(g_brcm_base + offset);
    mmio::write32(g_brcm_base + BRCM_EXT_CFG_INDEX, static_cast<uint32_t>(base));
    return mmio::read16(g_brcm_base + BRCM_EXT_CFG_DATA + offset);
}

__PRIVILEGED_CODE static uint32_t cfg_read32(uintptr_t base, uint16_t offset) {
    if (g_backend == config_backend::ECAM) {
        return mmio::read32(base + offset);
    }
    if (base == 0) return mmio::read32(g_brcm_base + offset);
    mmio::write32(g_brcm_base + BRCM_EXT_CFG_INDEX, static_cast<uint32_t>(base));
    return mmio::read32(g_brcm_base + BRCM_EXT_CFG_DATA + offset);
}

__PRIVILEGED_CODE static void cfg_write8(uintptr_t base, uint16_t offset, uint8_t val) {
    if (g_backend == config_backend::ECAM) {
        mmio::write8(base + offset, val); return;
    }
    if (base == 0) { mmio::write8(g_brcm_base + offset, val); return; }
    mmio::write32(g_brcm_base + BRCM_EXT_CFG_INDEX, static_cast<uint32_t>(base));
    mmio::write8(g_brcm_base + BRCM_EXT_CFG_DATA + offset, val);
}

__PRIVILEGED_CODE static void cfg_write16(uintptr_t base, uint16_t offset, uint16_t val) {
    if (g_backend == config_backend::ECAM) {
        mmio::write16(base + offset, val); return;
    }
    if (base == 0) { mmio::write16(g_brcm_base + offset, val); return; }
    mmio::write32(g_brcm_base + BRCM_EXT_CFG_INDEX, static_cast<uint32_t>(base));
    mmio::write16(g_brcm_base + BRCM_EXT_CFG_DATA + offset, val);
}

__PRIVILEGED_CODE static void cfg_write32(uintptr_t base, uint16_t offset, uint32_t val) {
    if (g_backend == config_backend::ECAM) {
        mmio::write32(base + offset, val); return;
    }
    if (base == 0) { mmio::write32(g_brcm_base + offset, val); return; }
    mmio::write32(g_brcm_base + BRCM_EXT_CFG_INDEX, static_cast<uint32_t>(base));
    mmio::write32(g_brcm_base + BRCM_EXT_CFG_DATA + offset, val);
}

__PRIVILEGED_CODE static bool brcm_link_up() {
    uint32_t status = mmio::read32(g_brcm_base + BRCM_PCIE_STATUS);
    return (status & BRCM_PHYLINKUP) && (status & BRCM_DL_ACTIVE);
}


const bar& device::get_bar(uint8_t index) const {
    if (index >= MAX_BARS) return g_null_bar;
    return m_bars[index];
}

bool device::has_capability(uint8_t cap_id) const {
    for (uint8_t i = 0; i < m_cap_count; i++) {
        if (m_caps[i].id == cap_id) return true;
    }
    return false;
}

uint8_t device::capability_offset(uint8_t cap_id) const {
    for (uint8_t i = 0; i < m_cap_count; i++) {
        if (m_caps[i].id == cap_id) return m_caps[i].offset;
    }
    return 0;
}

__PRIVILEGED_CODE uint8_t device::config_read8(uint16_t offset) const {
    return cfg_read8(m_ecam_base, offset);
}

__PRIVILEGED_CODE uint16_t device::config_read16(uint16_t offset) const {
    return cfg_read16(m_ecam_base, offset);
}

__PRIVILEGED_CODE uint32_t device::config_read32(uint16_t offset) const {
    return cfg_read32(m_ecam_base, offset);
}

__PRIVILEGED_CODE void device::config_write8(uint16_t offset, uint8_t val) {
    cfg_write8(m_ecam_base, offset, val);
}

__PRIVILEGED_CODE void device::config_write16(uint16_t offset, uint16_t val) {
    cfg_write16(m_ecam_base, offset, val);
}

__PRIVILEGED_CODE void device::config_write32(uint16_t offset, uint32_t val) {
    cfg_write32(m_ecam_base, offset, val);
}

__PRIVILEGED_CODE void device::enable() {
    uint16_t cmd = config_read16(CFG_COMMAND);
    cmd |= CMD_IO_SPACE | CMD_MEMORY_SPACE;
    config_write16(CFG_COMMAND, cmd);
}

__PRIVILEGED_CODE void device::enable_bus_mastering() {
    uint16_t cmd = config_read16(CFG_COMMAND);
    cmd |= CMD_BUS_MASTER;
    config_write16(CFG_COMMAND, cmd);
}

__PRIVILEGED_CODE void device::disable() {
    uint16_t cmd = config_read16(CFG_COMMAND);
    cmd &= ~static_cast<uint16_t>(CMD_IO_SPACE | CMD_MEMORY_SPACE | CMD_BUS_MASTER);
    config_write16(CFG_COMMAND, cmd);
}


__PRIVILEGED_CODE void parse_bars(device& dev) {
    if (dev.m_header_type != HDR_TYPE_NORMAL) return;

    uint16_t saved_cmd = dev.config_read16(CFG_COMMAND);
    dev.config_write16(CFG_COMMAND,
        saved_cmd & ~static_cast<uint16_t>(CMD_IO_SPACE | CMD_MEMORY_SPACE));

    for (uint8_t i = 0; i < MAX_BARS; i++) {
        uint16_t offset = CFG_BAR0 + static_cast<uint16_t>(i) * 4;
        uint32_t original = dev.config_read32(offset);

        if (original == 0) {
            dev.m_bars[i] = {0, 0, BAR_NONE, false};
            continue;
        }

        bool is_io = (original & BAR_IO_BIT) != 0;

        dev.config_write32(offset, BAR_PROBE_VALUE);
        uint32_t readback = dev.config_read32(offset);
        dev.config_write32(offset, original);

        if (is_io) {
            uint32_t masked = readback & BAR_ADDR_MASK_IO;
            if (masked == 0) { dev.m_bars[i] = {0, 0, BAR_NONE, false}; continue; }
            uint32_t sz = (~masked) + 1;
            uint64_t phys = original & BAR_ADDR_MASK_IO;
            dev.m_bars[i] = {phys, sz, BAR_IO, false};
        } else {
            bool is_64bit = (original & BAR_TYPE_MASK) == BAR_TYPE_64;
            bool prefetch = (original & BAR_PREFETCH_BIT) != 0;

            if (is_64bit && i < MAX_BARS - 1) {
                uint16_t offset_hi = offset + 4;
                uint32_t original_hi = dev.config_read32(offset_hi);
                dev.config_write32(offset_hi, BAR_PROBE_VALUE);
                uint32_t readback_hi = dev.config_read32(offset_hi);
                dev.config_write32(offset_hi, original_hi);

                uint64_t combined =
                    (static_cast<uint64_t>(readback_hi) << 32) |
                    (readback & BAR_ADDR_MASK_MEM);
                if (combined == 0) {
                    dev.m_bars[i] = {0, 0, BAR_NONE, false};
                    i++;
                    dev.m_bars[i] = {0, 0, BAR_NONE, false};
                    continue;
                }
                uint64_t sz = ~combined + 1;
                uint64_t phys =
                    (static_cast<uint64_t>(original_hi) << 32) |
                    (original & BAR_ADDR_MASK_MEM);
                dev.m_bars[i] = {phys, sz, BAR_MMIO64, prefetch};
                i++;
                dev.m_bars[i] = {0, 0, BAR_NONE, false};
            } else {
                uint32_t masked = readback & BAR_ADDR_MASK_MEM;
                if (masked == 0) { dev.m_bars[i] = {0, 0, BAR_NONE, false}; continue; }
                uint32_t sz = (~masked) + 1;
                uint64_t phys = original & BAR_ADDR_MASK_MEM;
                dev.m_bars[i] = {phys, sz, BAR_MMIO32, prefetch};
            }
        }
    }

    dev.config_write16(CFG_COMMAND, saved_cmd);
}


__PRIVILEGED_CODE void parse_capabilities(device& dev) {
    uint16_t status = dev.config_read16(CFG_STATUS);
    if (!(status & STS_CAPABILITIES)) return;

    uint8_t ptr = dev.config_read8(CFG_CAP_PTR) & CAP_PTR_MASK;
    if (ptr < CAP_MIN_OFFSET) return;

    uint32_t visited = 0;
    while (ptr != 0 && visited < MAX_CAP_WALK) {
        uint8_t id = dev.config_read8(ptr);
        uint8_t next = dev.config_read8(ptr + 1);

        if (dev.m_cap_count < MAX_CAPS) {
            dev.m_caps[dev.m_cap_count].id = id;
            dev.m_caps[dev.m_cap_count].offset = ptr;
            dev.m_cap_count++;
        }

        ptr = next & CAP_PTR_MASK;
        visited++;
    }
}


__PRIVILEGED_CODE void enumerate_function(uint8_t bus, uint8_t slot, uint8_t func) {
    if (g_device_count >= MAX_DEVICES) return;

    device& dev = g_devices[g_device_count];
    string::memset(&dev, 0, sizeof(device));

    dev.m_bus = bus;
    dev.m_slot = slot;
    dev.m_func = func;
    dev.m_ecam_base = dev_base_for(bus, slot, func);

    dev.m_vendor_id = cfg_read16(dev.m_ecam_base, CFG_VENDOR_ID);
    dev.m_device_id = cfg_read16(dev.m_ecam_base, CFG_DEVICE_ID);
    dev.m_class_code = cfg_read8(dev.m_ecam_base, CFG_CLASS_CODE);
    dev.m_subclass = cfg_read8(dev.m_ecam_base, CFG_SUBCLASS);
    dev.m_prog_if = cfg_read8(dev.m_ecam_base, CFG_PROG_IF);
    dev.m_header_type = cfg_read8(dev.m_ecam_base, CFG_HEADER_TYPE) & HDR_TYPE_MASK;
    dev.m_interrupt_pin = cfg_read8(dev.m_ecam_base, CFG_INTERRUPT_PIN);

    parse_bars(dev);
    parse_capabilities(dev);

    g_device_count++;
}

__PRIVILEGED_CODE static void enumerate_bus(uint8_t bus, uint8_t max_slots = SLOTS_PER_BUS) {
    for (uint8_t slot = 0; slot < max_slots; slot++) {
        uintptr_t fn0_base = dev_base_for(bus, slot, 0);
        uint16_t vendor = cfg_read16(fn0_base, CFG_VENDOR_ID);
        if (vendor == VENDOR_INVALID) continue;

        enumerate_function(bus, slot, 0);

        uint8_t hdr = cfg_read8(fn0_base, CFG_HEADER_TYPE);
        if (hdr & HDR_MULTI_FUNCTION) {
            for (uint8_t func = 1; func < FUNCS_PER_DEVICE; func++) {
                uintptr_t fn_base = dev_base_for(bus, slot, func);
                uint16_t fn_vendor = cfg_read16(fn_base, CFG_VENDOR_ID);
                if (fn_vendor != VENDOR_INVALID) {
                    enumerate_function(bus, slot, func);
                }
            }
        }
    }
}

// Follow PCI-to-PCI bridges to discover devices on secondary buses
__PRIVILEGED_CODE static void enumerate_bridges() {
    uint32_t initial_count = g_device_count;
    for (uint32_t i = 0; i < initial_count; i++) {
        device& dev = g_devices[i];
        if (dev.header_type() != HDR_TYPE_BRIDGE) continue;

        uint8_t secondary = dev.config_read8(CFG_SECONDARY_BUS);
        if (secondary == 0 || secondary == dev.bus()) continue;

        // For ECAM, ensure secondary bus is within the mapped range
        if (g_backend == config_backend::ECAM &&
            (secondary < g_ecam_start_bus || secondary > g_ecam_end_bus)) {
            continue;
        }

        if (g_backend == config_backend::BROADCOM && !brcm_link_up()) {
            log::warn("pci: PCIe link down, skipping bus %u", secondary);
            continue;
        }

        // PCIe links are point-to-point: only device 0 exists on secondary buses
        enumerate_bus(secondary, 1);
    }
}


__PRIVILEGED_CODE static int32_t init_ecam() {
    const auto* mcfg = acpi::find_table("MCFG");
    if (!mcfg) return ERR_NO_MCFG;

    uint32_t table_length = mcfg->length;
    if (table_length < sizeof(mcfg_table) + sizeof(mcfg_entry)) return ERR_NO_MCFG;

    auto* entries = reinterpret_cast<const mcfg_entry*>(
        reinterpret_cast<const uint8_t*>(mcfg) + sizeof(mcfg_table));
    const mcfg_entry& seg = entries[0];
    if (seg.base_address == 0) return ERR_NO_MCFG;

    uint16_t num_buses = static_cast<uint16_t>(seg.end_bus) - static_cast<uint16_t>(seg.start_bus) + 1;
    if (num_buses == 0) num_buses = 1;
    size_t ecam_size = static_cast<size_t>(num_buses) * 1024 * 1024;

    uintptr_t ecam_base_kva = 0, ecam_va_out = 0;
    int32_t rc = vmm::map_device(
        seg.base_address, ecam_size,
        paging::PAGE_READ | paging::PAGE_WRITE,
        ecam_base_kva, ecam_va_out);
    if (rc != vmm::OK) {
        log::warn("pci: failed to map ECAM at 0x%lx (%d)", seg.base_address, rc);
        return ERR_MAP;
    }

    g_backend = config_backend::ECAM;
    g_ecam_va = ecam_va_out;
    g_ecam_start_bus = seg.start_bus;
    g_ecam_end_bus = seg.end_bus;
    g_start_bus = seg.start_bus;

    log::info("pci: ECAM at phys 0x%lx, buses %u-%u", seg.base_address, seg.start_bus, seg.end_bus);
    return OK;
}


// BCM2711 (Raspberry Pi 4) known PCIe controller address
constexpr uint64_t BCM2711_PCIE_BASE = 0xfd500000;
constexpr uint64_t BCM2711_PCIE_SIZE = 0x9310;

__PRIVILEGED_CODE static bool is_rpi4_firmware() {
    const auto* fadt = acpi::find_table("FACP");
    if (!fadt) return false;
    return string::memcmp(fadt->oem_id, "RPIFDN", 6) == 0
        && string::memcmp(fadt->oem_table_id, "RPI4", 4) == 0;
}

__PRIVILEGED_CODE static int32_t init_broadcom() {
    uint64_t ctrl_base = 0, ctrl_size = 0;

    // Try DTB first
    int32_t fdt_rc = fdt::init();
    if (fdt_rc == fdt::OK) {
        int32_t node = fdt::find_compatible("brcm,bcm2711-pcie");
        if (node >= 0) {
            fdt::get_reg(node, &ctrl_base, &ctrl_size);
        }
    }

    if (ctrl_base == 0 && is_rpi4_firmware()) {
        log::info("pci: RPi4 detected via ACPI OEM, using known BCM2711 PCIe base");
        ctrl_base = BCM2711_PCIE_BASE;
        ctrl_size = BCM2711_PCIE_SIZE;
    }

    if (ctrl_base == 0 || ctrl_size == 0) return ERR_NO_MCFG;

    uintptr_t map_base = 0, map_va = 0;
    int32_t rc = vmm::map_device(
        ctrl_base, ctrl_size,
        paging::PAGE_READ | paging::PAGE_WRITE,
        map_base, map_va);
    if (rc != vmm::OK) {
        log::warn("pci: failed to map BCM2711 controller at 0x%lx (%d)", ctrl_base, rc);
        return ERR_MAP;
    }

    g_backend = config_backend::BROADCOM;
    g_brcm_base = map_va;
    g_start_bus = 0;

    log::info("pci: BCM2711 PCIe at phys 0x%lx", ctrl_base);
    return OK;
}


__PRIVILEGED_CODE int32_t init() {
    if (g_initialized) return OK;

    g_device_count = 0;

    // Try MCFG/ECAM first, then DTB/Broadcom
    int32_t rc = init_ecam();
    if (rc != OK) {
        rc = init_broadcom();
    }
    if (rc != OK) {
        log::warn("pci: no PCI host bridge found");
        return rc;
    }

    enumerate_bus(g_start_bus);
    enumerate_bridges();

    for (uint32_t i = 0; i < g_device_count; i++) {
        const device& d = g_devices[i];
        const char* name = device_class_name(d.class_code(), d.subclass(), d.prog_if());
        log::info("    %02x:%02x.%x %04x:%04x class %02x:%02x:%02x %s",
                  d.bus(), d.slot(), d.func(),
                  d.vendor_id(), d.device_id(),
                  d.class_code(), d.subclass(), d.prog_if(),
                  name);
    }

    log::info("pci: %u device(s) found", g_device_count);
    g_initialized = true;
    return OK;
}


uint32_t device_count() {
    return g_device_count;
}

device* get_device(uint32_t index) {
    if (index >= g_device_count) return nullptr;
    return &g_devices[index];
}

device* find_by_class(uint8_t class_code, uint8_t subclass) {
    for (uint32_t i = 0; i < g_device_count; i++) {
        if (g_devices[i].class_code() == class_code &&
            g_devices[i].subclass() == subclass) {
            return &g_devices[i];
        }
    }
    return nullptr;
}

device* find_by_progif(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    for (uint32_t i = 0; i < g_device_count; i++) {
        if (g_devices[i].class_code() == class_code &&
            g_devices[i].subclass() == subclass &&
            g_devices[i].prog_if() == prog_if) {
            return &g_devices[i];
        }
    }
    return nullptr;
}

uintptr_t brcm_controller_base() {
    return g_brcm_base;
}

} // namespace pci
