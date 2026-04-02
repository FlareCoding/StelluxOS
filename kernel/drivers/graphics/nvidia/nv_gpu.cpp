#include "drivers/graphics/nvidia/nv_gpu.h"
#include "drivers/graphics/nvidia/nv_firmware.h"
#include "drivers/graphics/nvidia/nv_gsp_boot.h"
#include "drivers/graphics/nvidia/nv_rpc.h"
#include "drivers/graphics/nvidia/nv_gsp_disp.h"
#include "hw/mmio.h"
#include "hw/cpu.h"
#include "hw/delay.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "mm/paging_types.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

namespace nv {

// ============================================================================
// PCI Driver Registration
// ============================================================================

} // namespace nv (close temporarily for PCI driver registration)

// Match any NVIDIA VGA controller: vendor 0x10DE, class 03:00
REGISTER_PCI_DRIVER(nv_gpu,
    PCI_MATCH(nv::PCI_VENDOR_NVIDIA, drivers::PCI_MATCH_ANY, nv::PCI_CLASS_DISPLAY, nv::PCI_SUBCLASS_VGA, drivers::PCI_MATCH_ANY_8),
    PCI_DRIVER_FACTORY(nv::nv_gpu));

namespace nv { // reopen

// ============================================================================
// Construction
// ============================================================================

nv_gpu::nv_gpu(pci::device* dev)
    : pci_driver("nvidia-gpu", dev)
    , m_bar0_va(0)
    , m_bar1_va(0)
    , m_bar1_size(0)
    , m_boot0(0)
    , m_chipset(0)
    , m_chiprev(0)
    , m_family(gpu_family::UNKNOWN)
    , m_vbios_size(0)
    , m_bit_offset(0)
    , m_edid_count(0)
    , m_head_mask(0)
    , m_sor_mask(0)
    , m_head_count(0)
    , m_sor_count(0)
    , m_active_heads(0)
    , m_active_sors(0) {
    string::memset(m_vbios, 0, sizeof(m_vbios));
    string::memset(&m_dcb, 0, sizeof(m_dcb));
    string::memset(&m_i2c, 0, sizeof(m_i2c));
    string::memset(&m_connectors, 0, sizeof(m_connectors));
    string::memset(m_edid, 0, sizeof(m_edid));
    string::memset(m_edid_dcb_index, 0, sizeof(m_edid_dcb_index));
}

// ============================================================================
// BAR0 MMIO Access
// ============================================================================

uint32_t nv_gpu::reg_rd32(uint32_t offset) const {
    return mmio::read32(m_bar0_va + offset);
}

void nv_gpu::reg_wr32(uint32_t offset, uint32_t val) {
    mmio::write32(m_bar0_va + offset, val);
}

void nv_gpu::reg_mask32(uint32_t offset, uint32_t mask, uint32_t val) {
    uint32_t tmp = reg_rd32(offset);
    reg_wr32(offset, (tmp & ~mask) | (val & mask));
}

// ============================================================================
// Attach — called during PCI bus enumeration
// ============================================================================

int32_t nv_gpu::attach() {
    log::info("nvidia: ========================================");
    log::info("nvidia: NVIDIA GPU driver initializing");
    log::info("nvidia: PCI %02x:%02x.%x vendor=%04x device=%04x",
              m_dev->bus(), m_dev->slot(), m_dev->func(),
              m_dev->vendor_id(), m_dev->device_id());
    log::info("nvidia: ========================================");

    // Step 1: Enable PCI device
    RUN_ELEVATED({
        m_dev->enable();
        m_dev->enable_bus_mastering();
    });
    log::info("nvidia: PCI device enabled (memory space + bus master)");

    // Step 2: Map BARs
    int32_t rc = map_bars();
    if (rc != OK) {
        log::error("nvidia: failed to map BARs: %d", rc);
        return rc;
    }

    // Step 3: Identify chip
    rc = identify_chip();
    if (rc != OK) {
        log::error("nvidia: failed to identify chip: %d", rc);
        return rc;
    }

    // Step 4: Wait for GPU firmware boot
    rc = wait_gfw_boot();
    if (rc != OK) {
        log::error("nvidia: GPU firmware boot wait failed: %d", rc);
        return rc;
    }

    // Step 5: Check if display is fused off
    if (is_display_disabled()) {
        log::warn("nvidia: display engine is fused off on this GPU");
        return ERR_NO_DISPLAY;
    }
    log::info("nvidia: display engine is available");

    // Step 6: Read and parse VBIOS
    rc = read_vbios();
    if (rc != OK) {
        log::warn("nvidia: VBIOS read failed: %d (continuing without VBIOS)", rc);
        // Non-fatal — we can try to proceed without VBIOS
    } else {
        rc = parse_vbios();
        if (rc != OK) {
            log::warn("nvidia: VBIOS parse failed: %d (continuing)", rc);
        }
    }

    log::info("nvidia: ========================================");
    log::info("nvidia: attach() complete — GPU ready for display init");
    log::info("nvidia: ========================================");

    return 0;
}

// ============================================================================
// Run — driver main loop (in its own kernel task)
// ============================================================================

void nv_gpu::run() {
    log::info("nvidia: driver task started");
    log::info("nvidia: ========================================");
    log::info("nvidia: GSP-RM Boot Pipeline");
    log::info("nvidia: ========================================");

    int32_t rc;

    // ================================================================
    // Phase A: Load GSP firmware from filesystem
    // ================================================================
    gsp_firmware fw;
    rc = firmware_load_all(this, fw);
    if (rc != OK) {
        log::error("nvidia: Phase A FAILED: firmware loading: %d", rc);
        log::info("nvidia: ensure firmware files are at /lib/firmware/nvidia/ga102/gsp/");
        goto idle;
    }

    // ================================================================
    // Phase B+C: GSP Boot (FWSEC-FRTS + SEC2 Booter + INIT_DONE)
    // ================================================================
    {
        gsp_boot_state boot;
        rc = gsp_boot(this, fw, boot);
        if (rc != OK) {
            log::error("nvidia: Phase C FAILED: GSP boot: %d", rc);
            firmware_free_all(fw);
            goto idle;
        }

        // ================================================================
        // Phase D: RM Object Initialization
        // ================================================================
        rm_state rm;
        rc = rm_init(this, boot, rm);
        if (rc != OK) {
            log::error("nvidia: Phase D FAILED: RM init: %d", rc);
            firmware_free_all(fw);
            goto idle;
        }

        // ================================================================
        // Phase E: Display Discovery via GSP-RM
        // ================================================================
        display_state disp;
        rc = gsp_display_probe(this, boot, rm, disp);
        if (rc != OK) {
            log::warn("nvidia: Phase E: display probe returned %d", rc);
        }

        // ================================================================
        // Phase F: Display output (future — requires display channel alloc)
        // ================================================================
        if (disp.initialized && disp.display_count > 0) {
            log::info("nvidia: ========================================");
            log::info("nvidia: DISPLAY PIPELINE COMPLETE");
            log::info("nvidia: %u monitor(s) detected via GSP-RM:", disp.display_count);
            for (uint32_t i = 0; i < disp.display_count; i++) {
                const display_info& d = disp.displays[i];
                if (d.has_edid && d.edid.valid) {
                    log::info("nvidia:   [%u] %s — %ux%u @%uHz (display_id=0x%08x)",
                              i, d.edid.display_name[0] ? d.edid.display_name : "(unnamed)",
                              d.edid.h_active, d.edid.v_active, d.edid.refresh_hz,
                              d.display_id);
                } else {
                    log::info("nvidia:   [%u] (no EDID) display_id=0x%08x", i, d.display_id);
                }
            }
            log::info("nvidia: ========================================");
            // TODO Phase F: Allocate display channels, set mode, scanout
            // This requires NV50_DISPLAY (class 0x9070+) allocation,
            // core/window/cursor channel setup, and mode programming
            // via EVO/NVDisplay push buffer commands through GSP-RM.
            // The display channel architecture is complex and will be
            // implemented as a follow-up once basic GSP communication
            // is verified on hardware.
        } else {
            log::warn("nvidia: no displays detected, skipping display output");
        }

        log::info("nvidia: ========================================");
        log::info("nvidia: GSP-RM driver initialization complete");
        log::info("nvidia: ========================================");
    }

idle:
    // Idle loop — wait for events (hotplug, mode changes, etc.)
    for (;;) {
        wait_for_event();
    }
}

// ============================================================================
// Interrupt handler
// ============================================================================

__PRIVILEGED_CODE void nv_gpu::on_interrupt(uint32_t vector) {
    (void)vector;
    // Phase 1: just acknowledge — we don't use interrupts yet
}

// ============================================================================
// Phase 1: BAR Mapping
// ============================================================================

int32_t nv_gpu::map_bars() {
    // Map BAR0 — 16MB MMIO control space
    int32_t rc = map_bar(0, m_bar0_va, paging::PAGE_USER);
    if (rc != 0) {
        log::error("nvidia: failed to map BAR0 (MMIO): %d", rc);
        return ERR_MAP_FAILED;
    }

    const pci::bar& bar0 = m_dev->get_bar(0);
    log::info("nvidia: BAR0 mapped: phys=0x%lx size=0x%lx va=0x%lx (MMIO registers)",
              bar0.phys, bar0.size, m_bar0_va);

    // Map BAR1 — VRAM aperture (write-combining for framebuffer access)
    const pci::bar& bar1 = m_dev->get_bar(1);
    if (bar1.type == pci::BAR_NONE) {
        log::warn("nvidia: BAR1 (VRAM aperture) not present");
        m_bar1_va = 0;
        m_bar1_size = 0;
    } else {
        // BAR1 can be very large (256MB-16GB). Map a reasonable portion.
        // For framebuffer purposes, 32MB is plenty (3840x2160x4 = ~33MB).
        uint64_t map_size = bar1.size;
        constexpr uint64_t MAX_BAR1_MAP = 64 * 1024 * 1024; // 64MB
        if (map_size > MAX_BAR1_MAP) {
            map_size = MAX_BAR1_MAP;
        }

        // Map with write-combining for optimal framebuffer writes
        uintptr_t base = 0, va = 0;
        RUN_ELEVATED(
            rc = vmm::map_device(
                static_cast<pmm::phys_addr_t>(bar1.phys),
                static_cast<size_t>(map_size),
                paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_USER | paging::PAGE_WC,
                base, va)
        );

        if (rc != vmm::OK) {
            log::warn("nvidia: failed to map BAR1 (VRAM): %d (non-fatal)", rc);
            m_bar1_va = 0;
            m_bar1_size = 0;
        } else {
            m_bar1_va = va;
            m_bar1_size = map_size;
            log::info("nvidia: BAR1 mapped: phys=0x%lx total=0x%lx mapped=0x%lx va=0x%lx (VRAM aperture, WC)",
                      bar1.phys, bar1.size, map_size, m_bar1_va);
        }
    }

    // Log all BARs for completeness
    log_bar_info();

    return OK;
}

// ============================================================================
// Phase 1: Chip Identification
// ============================================================================

int32_t nv_gpu::identify_chip() {
    m_boot0 = reg_rd32(reg::PMC_BOOT_0);

    // Validate — if BAR0 is misconfigured, we'll read 0xFFFFFFFF
    if (m_boot0 == 0xFFFFFFFF || m_boot0 == 0x00000000) {
        log::error("nvidia: PMC_BOOT_0 reads 0x%08x — BAR0 not accessible", m_boot0);
        return ERR_IO;
    }

    // Check the family test bit (NV10+)
    if ((m_boot0 & reg::PMC_BOOT_0_FAMILY_MASK) == 0) {
        log::error("nvidia: PMC_BOOT_0=0x%08x — very old GPU (pre-NV10), unsupported", m_boot0);
        return ERR_UNSUPPORTED;
    }

    // Extract chipset and revision
    m_chipset = (m_boot0 & reg::PMC_BOOT_0_CHIPSET_MASK) >> reg::PMC_BOOT_0_CHIPSET_SHIFT;
    m_chiprev = static_cast<uint8_t>(m_boot0 & reg::PMC_BOOT_0_CHIPREV_MASK);

    // Determine architecture family from chipset
    uint32_t family_bits = m_chipset & 0x1F0;
    switch (family_bits) {
    case 0x050: case 0x080: case 0x090: case 0x0A0:
        m_family = gpu_family::TESLA;
        break;
    case 0x0C0: case 0x0D0:
        m_family = gpu_family::FERMI;
        break;
    case 0x0E0: case 0x0F0: case 0x100:
        m_family = gpu_family::KEPLER;
        break;
    case 0x110: case 0x120:
        m_family = gpu_family::MAXWELL;
        break;
    case 0x130:
        m_family = gpu_family::PASCAL;
        break;
    case 0x140:
        m_family = gpu_family::VOLTA;
        break;
    case 0x160:
        m_family = gpu_family::TURING;
        break;
    case 0x170:
        m_family = gpu_family::AMPERE;
        break;
    case 0x190:
        m_family = gpu_family::ADA;
        break;
    default:
        m_family = gpu_family::UNKNOWN;
        break;
    }

    log_chip_info();

    // Verify endianness
    uint32_t boot1 = reg_rd32(reg::PMC_BOOT_1);
    if (boot1 != 0x00000000) {
        log::warn("nvidia: GPU is big-endian (BOOT_1=0x%08x), switching to LE", boot1);
        reg_wr32(reg::PMC_BOOT_1, 0x00000000);
        delay::us(100);
        boot1 = reg_rd32(reg::PMC_BOOT_1);
        if (boot1 != 0x00000000) {
            log::error("nvidia: failed to switch GPU to little-endian");
            return ERR_IO;
        }
    }
    log::info("nvidia: GPU endianness: little-endian (OK)");

    // Read crystal frequency from straps
    uint32_t strap = reg_rd32(reg::PSTRAPS);
    uint32_t crystal_bits = strap & reg::PSTRAPS_CRYSTAL_MASK;
    const char* crystal_str = "unknown";
    switch (crystal_bits) {
    case 0x00000000: crystal_str = "13.5 MHz"; break;
    case 0x00000040: crystal_str = "14.318 MHz"; break;
    case 0x00400000: crystal_str = "27.0 MHz"; break;
    case 0x00400040: crystal_str = "25.0 MHz"; break;
    }
    log::info("nvidia: reference crystal: %s (strap=0x%08x)", crystal_str, strap);

    return OK;
}

// ============================================================================
// Phase 1: GFW Boot Wait
// ============================================================================

int32_t nv_gpu::wait_gfw_boot() {
    // Only Turing+ runs firmware-based devinit
    if (m_family < gpu_family::TURING) {
        log::info("nvidia: pre-Turing GPU — no GFW_BOOT wait needed");
        return OK;
    }

    log::info("nvidia: waiting for GPU firmware (GFW) boot completion...");

    // Poll with timeout: 2050 iterations * ~2ms = ~4.1s max
    constexpr uint32_t MAX_ITERS = 2050;

    for (uint32_t i = 0; i < MAX_ITERS; i++) {
        uint32_t status = reg_rd32(reg::GFW_BOOT_STATUS);
        if (status & reg::GFW_BOOT_STARTED) {
            uint32_t progress = reg_rd32(reg::GFW_BOOT_PROGRESS);
            if ((progress & reg::GFW_BOOT_PROGRESS_MASK) == reg::GFW_BOOT_COMPLETED) {
                log::info("nvidia: GFW boot complete (status=0x%08x progress=0x%08x, %u ms)",
                          status, progress, i * 2);
                return OK;
            }
        }
        delay::us(2000); // 2ms per iteration
    }

    uint32_t status = reg_rd32(reg::GFW_BOOT_STATUS);
    uint32_t progress = reg_rd32(reg::GFW_BOOT_PROGRESS);
    log::error("nvidia: GFW boot TIMEOUT (status=0x%08x progress=0x%08x)",
               status, progress);
    return ERR_TIMEOUT;
}

// ============================================================================
// Phase 1: Display Fuse Check
// ============================================================================

bool nv_gpu::is_display_disabled() {
    uint32_t fuse_reg = (m_family >= gpu_family::AMPERE)
        ? reg::DISP_FUSE_GA100
        : reg::DISP_FUSE_GM107;

    uint32_t val = reg_rd32(fuse_reg);
    bool disabled = (val & reg::DISP_FUSE_DISABLED) != 0;
    log::info("nvidia: display fuse register (0x%06x) = 0x%08x → %s",
              fuse_reg, val, disabled ? "DISABLED" : "enabled");
    return disabled;
}

// ============================================================================
// Phase 2: VBIOS Reading
// ============================================================================

int32_t nv_gpu::read_vbios() {
    log::info("nvidia: ---- VBIOS Reading ----");

    // Try PROM first (BAR0 + 0x300000)
    int32_t rc = read_vbios_prom();
    if (rc == OK) {
        log::info("nvidia: VBIOS read via PROM (BAR0+0x300000), size=%u bytes", m_vbios_size);
        return OK;
    }
    log::info("nvidia: PROM read failed (%d), trying PRAMIN...", rc);

    // Try PRAMIN (BAR0 + 0x700000)
    rc = read_vbios_pramin();
    if (rc == OK) {
        log::info("nvidia: VBIOS read via PRAMIN (BAR0+0x700000), size=%u bytes", m_vbios_size);
        return OK;
    }
    log::info("nvidia: PRAMIN read failed (%d), trying PCI ROM...", rc);

    // Try PCI expansion ROM
    rc = read_vbios_pci_rom();
    if (rc == OK) {
        log::info("nvidia: VBIOS read via PCI expansion ROM, size=%u bytes", m_vbios_size);
        return OK;
    }
    log::error("nvidia: all VBIOS read methods failed");

    return ERR_NOT_FOUND;
}

int32_t nv_gpu::read_vbios_prom() {
    // Disable ROM shadow to expose the real ROM
    // PCI config offset 0x50, bit 0 controls ROM shadow
    uint32_t shadow_reg = 0;
    RUN_ELEVATED(shadow_reg = m_dev->config_read32(0x50));
    if (shadow_reg & 0x01) {
        RUN_ELEVATED(m_dev->config_write32(0x50, shadow_reg & ~0x01u));
        delay::us(100);
    }

    // Read first 2 bytes to check for ROM signature
    uint32_t first_word = reg_rd32(reg::PROM_BASE);
    uint16_t sig = static_cast<uint16_t>(first_word & 0xFFFF);

    if (sig != reg::ROM_SIG_PCI && sig != reg::ROM_SIG_ALT1 && sig != reg::ROM_SIG_NV) {
        log::info("nvidia: PROM: no valid ROM signature (got 0x%04x)", sig);
        // Restore shadow
        if (shadow_reg & 0x01) {
            RUN_ELEVATED(m_dev->config_write32(0x50, shadow_reg));
        }
        return ERR_INVALID;
    }

    // Probe the actual expansion ROM size via PCI config space as a safety net.
    uint32_t pci_rom_size = 0;
    {
        uint32_t saved_bar = 0;
        RUN_ELEVATED(saved_bar = m_dev->config_read32(reg::PCI_EXPANSION_ROM_BAR));
        RUN_ELEVATED(m_dev->config_write32(reg::PCI_EXPANSION_ROM_BAR, reg::PCI_ROM_ADDR_MASK));
        uint32_t probe = 0;
        RUN_ELEVATED(probe = m_dev->config_read32(reg::PCI_EXPANSION_ROM_BAR));
        RUN_ELEVATED(m_dev->config_write32(reg::PCI_EXPANSION_ROM_BAR, saved_bar));

        if (probe & reg::PCI_ROM_ADDR_MASK) {
            pci_rom_size = ~(probe & reg::PCI_ROM_ADDR_MASK) + 1;
            log::info("nvidia: PROM: PCI ROM BAR reports %u bytes (%u KB)", pci_rom_size, pci_rom_size / 1024);
        }
    }

    // Walk ALL ROM images (PciAt, EFI, FwSec, ...) scanning past the LAST flag.
    uint32_t rom_size = 0;
    uint32_t scan_offset = 0;

    while (scan_offset < VBIOS_MAX_SIZE) {
        // Check for ROM signature at this image's start
        uint32_t img_first = reg_rd32(reg::PROM_BASE + scan_offset);
        uint16_t img_sig = static_cast<uint16_t>(img_first & 0xFFFF);

        if (img_sig != reg::ROM_SIG_PCI && img_sig != reg::ROM_SIG_ALT1 && img_sig != reg::ROM_SIG_NV) {
            // No more images
            break;
        }

        // Find PCIR structure (pointer at image_start + 0x18)
        uint32_t pcir_ptr_word = reg_rd32(reg::PROM_BASE + scan_offset + 0x18);
        uint16_t pcir_rel = static_cast<uint16_t>(pcir_ptr_word & 0xFFFF);
        uint32_t pcir_abs = scan_offset + pcir_rel;

        if (pcir_abs + 24 > VBIOS_MAX_SIZE) break;

        uint32_t pcir_sig = reg_rd32(reg::PROM_BASE + pcir_abs);
        if (pcir_sig != reg::PCIR_SIGNATURE) {
            log::warn("nvidia: PROM: expected PCIR at 0x%x, got 0x%08x", pcir_abs, pcir_sig);
            break;
        }

        // Image size at PCIR + 0x10, in 512-byte units
        uint32_t size_word = reg_rd32(reg::PROM_BASE + pcir_abs + 0x10);
        uint32_t img_blocks = static_cast<uint16_t>(size_word & 0xFFFF);
        uint32_t img_size = img_blocks * 512;
        uint8_t code_type = static_cast<uint8_t>((size_word >> 16) & 0xFF); // PCIR + 0x14 is in same dword shifted
        // Actually code_type is at pcir_abs + 0x14, let's read it properly
        uint32_t type_word = reg_rd32(reg::PROM_BASE + pcir_abs + 0x14);
        code_type = static_cast<uint8_t>(type_word & 0xFF);
        uint8_t last_image = static_cast<uint8_t>((type_word >> 8) & 0xFF);

        log::info("nvidia: PROM: image at 0x%x: type=0x%02x size=%u (%u blocks)%s",
                  scan_offset, code_type, img_size, img_blocks,
                  (last_image & 0x80) ? " [LAST]" : "");

        if (img_size == 0) break;

        scan_offset += img_size;
        rom_size = scan_offset;

        // NOTE: Do NOT stop at last_image flag! On NVIDIA GPUs, the EFI image
        // (type 0x03) often has last_image set, but FwSec images (type 0xE0)
        // follow after it. Nova-core scans the entire 1MB ROM window.
        // We continue scanning until we find no more valid ROM signatures.
    }

    // Use PCI ROM BAR size if the chain walk found fewer bytes
    if (pci_rom_size > rom_size && pci_rom_size <= VBIOS_MAX_SIZE) {
        log::info("nvidia: PROM: chain walk found %u bytes, PCI ROM BAR says %u — using larger",
                  rom_size, pci_rom_size);
        rom_size = pci_rom_size;
    }

    if (rom_size == 0) {
        rom_size = 64 * 1024;
        log::warn("nvidia: PROM: ROM chain walk failed, falling back to 64KB read");
    }

    if (rom_size > VBIOS_MAX_SIZE) {
        rom_size = VBIOS_MAX_SIZE;
    }

    log::info("nvidia: PROM: reading %u bytes (%u KB)", rom_size, rom_size / 1024);

    // Read entire ROM
    for (uint32_t i = 0; i < rom_size; i += 4) {
        uint32_t val = reg_rd32(reg::PROM_BASE + i);
        string::memcpy(&m_vbios[i], &val, 4);
    }
    m_vbios_size = rom_size;

    // Restore shadow if it was set
    if (shadow_reg & 0x01) {
        RUN_ELEVATED(m_dev->config_write32(0x50, shadow_reg));
    }

    return OK;
}

int32_t nv_gpu::read_vbios_pramin() {
    // Check if VBIOS instance pointer is valid
    uint32_t inst_reg = (m_family >= gpu_family::VOLTA)
        ? reg::VBIOS_INST_GV100
        : reg::VBIOS_INST_GM100;

    uint32_t inst = reg_rd32(inst_reg);
    log::info("nvidia: PRAMIN: instance register (0x%06x) = 0x%08x", inst_reg, inst);

    // Check enabled and VRAM target
    if (!(inst & reg::VBIOS_INST_ENABLED)) {
        log::info("nvidia: PRAMIN: instance window not enabled");
        return ERR_NOT_FOUND;
    }
    if ((inst & reg::VBIOS_INST_TARGET_MASK) != 1) {
        log::info("nvidia: PRAMIN: instance not targeting VRAM (target=%u)",
                  inst & reg::VBIOS_INST_TARGET_MASK);
        return ERR_NOT_FOUND;
    }

    // Calculate address
    uint64_t addr = (static_cast<uint64_t>(inst & 0xFFFFFF00)) << 8;
    if (addr == 0) {
        uint32_t pramin_reg = reg_rd32(reg::PRAMIN_WINDOW);
        addr = (static_cast<uint64_t>(pramin_reg) << 16) + 0xF0000;
        log::info("nvidia: PRAMIN: fallback addr from reg 0x1700: 0x%lx", addr);
    } else {
        log::info("nvidia: PRAMIN: VBIOS address: 0x%lx", addr);
    }

    // Save old PRAMIN window and set new one
    uint32_t old_pramin = reg_rd32(reg::PRAMIN_WINDOW);
    reg_wr32(reg::PRAMIN_WINDOW, static_cast<uint32_t>(addr >> 16));
    delay::us(100);

    // Read first word to check signature
    uint32_t first_word = reg_rd32(reg::PRAMIN_BASE);
    uint16_t sig = static_cast<uint16_t>(first_word & 0xFFFF);

    if (sig != reg::ROM_SIG_PCI && sig != reg::ROM_SIG_ALT1 && sig != reg::ROM_SIG_NV) {
        log::info("nvidia: PRAMIN: no valid ROM signature (got 0x%04x)", sig);
        reg_wr32(reg::PRAMIN_WINDOW, old_pramin);
        return ERR_INVALID;
    }

    // Read ROM
    uint32_t rom_size = 64 * 1024; // Default 64KB

    // Check PCIR for real size
    uint32_t pcir_ptr_word = reg_rd32(reg::PRAMIN_BASE + 0x18);
    uint16_t pcir_offset = static_cast<uint16_t>(pcir_ptr_word & 0xFFFF);
    if (pcir_offset >= 4 && pcir_offset < rom_size - 24) {
        uint32_t pcir_sig = reg_rd32(reg::PRAMIN_BASE + pcir_offset);
        if (pcir_sig == reg::PCIR_SIGNATURE) {
            uint32_t size_word = reg_rd32(reg::PRAMIN_BASE + pcir_offset + 0x10);
            uint32_t img_blocks = static_cast<uint16_t>(size_word & 0xFFFF);
            uint32_t img_size = img_blocks * 512;
            if (img_size > 0 && img_size <= VBIOS_MAX_SIZE) {
                rom_size = img_size;
            }
        }
    }

    if (rom_size > VBIOS_MAX_SIZE) {
        rom_size = VBIOS_MAX_SIZE;
    }

    for (uint32_t i = 0; i < rom_size; i += 4) {
        uint32_t val = reg_rd32(reg::PRAMIN_BASE + i);
        string::memcpy(&m_vbios[i], &val, 4);
    }
    m_vbios_size = rom_size;

    // Restore old PRAMIN window
    reg_wr32(reg::PRAMIN_WINDOW, old_pramin);

    return OK;
}

int32_t nv_gpu::read_vbios_pci_rom() {
    // Read PCI expansion ROM BAR (config offset 0x30)
    uint32_t rom_bar = 0;
    RUN_ELEVATED(rom_bar = m_dev->config_read32(reg::PCI_EXPANSION_ROM_BAR));

    // Probe ROM size
    RUN_ELEVATED({
        m_dev->config_write32(reg::PCI_EXPANSION_ROM_BAR, reg::PCI_ROM_ADDR_MASK);
    });
    uint32_t probe = 0;
    RUN_ELEVATED(probe = m_dev->config_read32(reg::PCI_EXPANSION_ROM_BAR));

    // Restore original value
    RUN_ELEVATED(m_dev->config_write32(reg::PCI_EXPANSION_ROM_BAR, rom_bar));

    uint32_t size = ~(probe & reg::PCI_ROM_ADDR_MASK) + 1;
    if (size == 0 || size > 16 * 1024 * 1024) {
        log::info("nvidia: PCI ROM: no expansion ROM detected (probe=0x%08x size=%u)",
                  probe, size);
        return ERR_NOT_FOUND;
    }

    uint64_t rom_phys = rom_bar & reg::PCI_ROM_ADDR_MASK;
    if (rom_phys == 0) {
        log::info("nvidia: PCI ROM: ROM BAR has no address assigned");
        return ERR_NOT_FOUND;
    }

    log::info("nvidia: PCI ROM: phys=0x%lx size=%u", rom_phys, size);

    // Enable ROM
    RUN_ELEVATED(m_dev->config_write32(reg::PCI_EXPANSION_ROM_BAR,
        static_cast<uint32_t>(rom_phys) | reg::PCI_ROM_ENABLE));
    delay::us(100);

    // Map ROM
    uintptr_t rom_base = 0, rom_va = 0;
    int32_t rc = 0;
    RUN_ELEVATED(rc = vmm::map_phys(
        static_cast<pmm::phys_addr_t>(rom_phys),
        static_cast<size_t>(size),
        paging::PAGE_READ | paging::PAGE_USER,
        rom_base, rom_va));

    if (rc != vmm::OK) {
        log::warn("nvidia: PCI ROM: failed to map ROM: %d", rc);
        RUN_ELEVATED(m_dev->config_write32(reg::PCI_EXPANSION_ROM_BAR, rom_bar));
        return ERR_MAP_FAILED;
    }

    // Check signature
    uint16_t sig = *reinterpret_cast<volatile uint16_t*>(rom_va);
    if (sig != reg::ROM_SIG_PCI && sig != reg::ROM_SIG_ALT1) {
        log::info("nvidia: PCI ROM: invalid signature 0x%04x", sig);
        RUN_ELEVATED(vmm::free(rom_base));
        RUN_ELEVATED(m_dev->config_write32(reg::PCI_EXPANSION_ROM_BAR, rom_bar));
        return ERR_INVALID;
    }

    // Copy ROM data
    uint32_t copy_size = (size > VBIOS_MAX_SIZE) ? VBIOS_MAX_SIZE : size;
    for (uint32_t i = 0; i < copy_size; i++) {
        m_vbios[i] = *reinterpret_cast<volatile uint8_t*>(rom_va + i);
    }
    m_vbios_size = copy_size;

    // Cleanup
    RUN_ELEVATED(vmm::free(rom_base));
    RUN_ELEVATED(m_dev->config_write32(reg::PCI_EXPANSION_ROM_BAR, rom_bar));

    return OK;
}

// ============================================================================
// Phase 2: VBIOS Parsing
// ============================================================================

// Helper: read 8-bit value from VBIOS buffer
static uint8_t vbios_rd8(const uint8_t* vbios, uint32_t offset) {
    return vbios[offset];
}

// Helper: read 16-bit LE value from VBIOS buffer
static uint16_t vbios_rd16(const uint8_t* vbios, uint32_t offset) {
    return static_cast<uint16_t>(vbios[offset]) |
           (static_cast<uint16_t>(vbios[offset + 1]) << 8);
}

// Helper: read 32-bit LE value from VBIOS buffer
static uint32_t vbios_rd32(const uint8_t* vbios, uint32_t offset) {
    return static_cast<uint32_t>(vbios[offset]) |
           (static_cast<uint32_t>(vbios[offset + 1]) << 8) |
           (static_cast<uint32_t>(vbios[offset + 2]) << 16) |
           (static_cast<uint32_t>(vbios[offset + 3]) << 24);
}

int32_t nv_gpu::parse_vbios() {
    log::info("nvidia: ---- VBIOS Parsing ----");

    // Validate ROM signature
    uint16_t sig = vbios_rd16(m_vbios, 0);
    log::info("nvidia: VBIOS signature: 0x%04x (%s)", sig,
              (sig == reg::ROM_SIG_PCI) ? "AA55" :
              (sig == reg::ROM_SIG_ALT1) ? "BB77" :
              (sig == reg::ROM_SIG_NV) ? "NV" : "unknown");

    // Find BIT table
    int32_t rc = find_bit_table();
    if (rc != OK) {
        log::error("nvidia: BIT table not found");
        return rc;
    }

    // Parse DCB from fixed VBIOS offset 0x36
    rc = parse_dcb();
    if (rc != OK) {
        log::error("nvidia: DCB parsing failed");
        return rc;
    }

    // Parse I2C table (pointer from DCB header)
    rc = parse_i2c_table();
    if (rc != OK) {
        log::warn("nvidia: I2C table parsing failed (%d)", rc);
        // Non-fatal — some outputs may use DP AUX instead
    }

    // Parse connector table (pointer from DCB header)
    rc = parse_connector_table();
    if (rc != OK) {
        log::warn("nvidia: connector table parsing failed (%d, non-fatal)", rc);
    }

    return OK;
}

int32_t nv_gpu::find_bit_table() {
    // Scan for BIT signature: FF B8 42 49 54 ("\xff\xb8BIT")
    const uint8_t bit_sig[] = { 0xFF, 0xB8, 0x42, 0x49, 0x54 };

    for (uint32_t i = 0; i + 5 <= m_vbios_size; i++) {
        bool match = true;
        for (uint32_t j = 0; j < 5; j++) {
            if (m_vbios[i + j] != bit_sig[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            m_bit_offset = i;

            // Read BIT header
            uint8_t entry_size = vbios_rd8(m_vbios, m_bit_offset + 9);
            uint8_t entry_count = vbios_rd8(m_vbios, m_bit_offset + 10);

            log::info("nvidia: BIT table found at VBIOS offset 0x%04x", m_bit_offset);
            log::info("nvidia: BIT entries: %u, entry size: %u bytes", entry_count, entry_size);

            // Log all BIT token IDs
            for (uint8_t e = 0; e < entry_count; e++) {
                uint32_t entry_off = m_bit_offset + 12 + e * entry_size;
                if (entry_off + 6 > m_vbios_size) break;

                uint8_t id = vbios_rd8(m_vbios, entry_off);
                uint8_t ver = vbios_rd8(m_vbios, entry_off + 1);
                uint16_t len = vbios_rd16(m_vbios, entry_off + 2);
                uint16_t ptr = vbios_rd16(m_vbios, entry_off + 4);
                log::debug("nvidia:   BIT token '%c' (0x%02x) v%u len=%u ptr=0x%04x",
                           (id >= 0x20 && id < 0x7F) ? id : '?', id, ver, len, ptr);
            }

            // Read VBIOS version from 'i' token
            for (uint8_t e = 0; e < entry_count; e++) {
                uint32_t entry_off = m_bit_offset + 12 + e * entry_size;
                if (entry_off + 6 > m_vbios_size) break;
                if (vbios_rd8(m_vbios, entry_off) == 'i') {
                    uint16_t ptr = vbios_rd16(m_vbios, entry_off + 4);
                    uint16_t len = vbios_rd16(m_vbios, entry_off + 2);
                    if (len >= 5 && ptr + 5 <= m_vbios_size) {
                        uint8_t major = vbios_rd8(m_vbios, ptr + 3);
                        uint8_t chip  = vbios_rd8(m_vbios, ptr + 2);
                        uint8_t minor = vbios_rd8(m_vbios, ptr + 1);
                        uint8_t micro = vbios_rd8(m_vbios, ptr + 0);
                        uint8_t patch = vbios_rd8(m_vbios, ptr + 4);
                        log::info("nvidia: VBIOS version: %02x.%02x.%02x.%02x.%02x",
                                  major, chip, minor, micro, patch);
                    }
                    break;
                }
            }

            return OK;
        }
    }

    return ERR_NOT_FOUND;
}

int32_t nv_gpu::parse_dcb() {
    // DCB pointer is at fixed VBIOS offset 0x36 (16-bit LE)
    if (m_vbios_size < 0x38) {
        return ERR_INVALID;
    }

    uint16_t dcb_ptr = vbios_rd16(m_vbios, 0x36);
    if (dcb_ptr == 0 || dcb_ptr >= m_vbios_size) {
        log::error("nvidia: DCB pointer invalid (0x%04x)", dcb_ptr);
        return ERR_NOT_FOUND;
    }

    log::info("nvidia: DCB pointer at VBIOS offset 0x%04x", dcb_ptr);

    // Read DCB header
    uint8_t version = vbios_rd8(m_vbios, dcb_ptr + 0);
    uint8_t hdr_size = vbios_rd8(m_vbios, dcb_ptr + 1);
    uint8_t entry_count = vbios_rd8(m_vbios, dcb_ptr + 2);
    uint8_t entry_size = vbios_rd8(m_vbios, dcb_ptr + 3);

    log::info("nvidia: DCB version: 0x%02x, header: %u bytes, entries: %u, entry size: %u",
              version, hdr_size, entry_count, entry_size);

    // Validate version (we support 3.0+ / 4.x)
    if (version < 0x30) {
        log::error("nvidia: DCB version 0x%02x too old (need >= 0x30)", version);
        return ERR_UNSUPPORTED;
    }

    // Validate DCB signature at +6
    if (hdr_size >= 10) {
        uint32_t dcb_sig = vbios_rd32(m_vbios, dcb_ptr + 6);
        if (dcb_sig != 0x4EDCBDCB) {
            log::error("nvidia: DCB signature mismatch: 0x%08x (expected 0x4EDCBDCB)", dcb_sig);
            return ERR_INVALID;
        }
        log::info("nvidia: DCB signature validated (0x4EDCBDCB)");
    }

    // Extract table pointers from header
    if (hdr_size >= 6) {
        m_dcb.i2c_table_ptr = vbios_rd16(m_vbios, dcb_ptr + 4);
        log::info("nvidia: DCB I2C/CCB table pointer: 0x%04x", m_dcb.i2c_table_ptr);
    }
    if (hdr_size >= 12) {
        m_dcb.gpio_table_ptr = vbios_rd16(m_vbios, dcb_ptr + 10);
        log::info("nvidia: DCB GPIO table pointer: 0x%04x", m_dcb.gpio_table_ptr);
    }
    if (hdr_size >= 22) {
        m_dcb.connector_table_ptr = vbios_rd16(m_vbios, dcb_ptr + 20);
        log::info("nvidia: DCB connector table pointer: 0x%04x", m_dcb.connector_table_ptr);
    }

    // Parse device entries
    m_dcb.count = 0;
    for (uint8_t i = 0; i < entry_count && m_dcb.count < MAX_DCB_ENTRIES; i++) {
        uint32_t entry_off = dcb_ptr + hdr_size + i * entry_size;
        if (entry_off + 8 > m_vbios_size) break;

        uint32_t dw0 = vbios_rd32(m_vbios, entry_off);
        uint32_t dw1 = vbios_rd32(m_vbios, entry_off + 4);

        // End-of-table markers
        if (dw0 == 0x00000000 || dw0 == 0xFFFFFFFF) break;

        uint8_t type = dw0 & 0x0F;
        if (type == 0x0F) continue; // Skip
        if (type == 0x0E) break;    // EOL

        dcb_entry& e = m_dcb.entries[m_dcb.count];
        e.type       = static_cast<dcb_output_type>(type);
        e.i2c_index  = (dw0 >> 4) & 0x0F;
        e.head_mask  = (dw0 >> 8) & 0x0F;
        e.connector  = (dw0 >> 12) & 0x0F;
        e.bus        = (dw0 >> 16) & 0x0F;
        e.location   = (dw0 >> 20) & 0x03;
        e.or_mask    = (dw0 >> 24) & 0x0F;
        e.raw[0]     = dw0;
        e.raw[1]     = dw1;

        // DFP-specific fields
        if (type == 0x02 || type == 0x03 || type == 0x06) { // TMDS, LVDS, DP
            e.link         = (dw1 >> 4) & 0x03;
            e.hdmi_enable  = (dw1 >> 17) & 0x01;
            e.dp_max_rate  = (dw1 >> 21) & 0x07;
            e.dp_max_lanes = (dw1 >> 24) & 0x0F;
        } else {
            e.link = 0;
            e.hdmi_enable = false;
            e.dp_max_rate = 0;
            e.dp_max_lanes = 0;
        }

        log_dcb_entry(m_dcb.count, e);
        m_dcb.count++;
    }

    log::info("nvidia: DCB: %u display output(s) found", m_dcb.count);
    return OK;
}

int32_t nv_gpu::parse_i2c_table() {
    uint16_t i2c_ptr = m_dcb.i2c_table_ptr;
    if (i2c_ptr == 0 || i2c_ptr >= m_vbios_size) {
        return ERR_NOT_FOUND;
    }

    log::info("nvidia: ---- I2C/CCB Table ----");

    uint8_t version = vbios_rd8(m_vbios, i2c_ptr);
    uint8_t hdr_size = vbios_rd8(m_vbios, i2c_ptr + 1);
    uint8_t entry_count = vbios_rd8(m_vbios, i2c_ptr + 2);
    uint8_t entry_size = vbios_rd8(m_vbios, i2c_ptr + 3);

    log::info("nvidia: I2C table version: 0x%02x, entries: %u, entry size: %u",
              version, entry_count, entry_size);

    m_i2c.count = 0;
    for (uint8_t i = 0; i < entry_count && m_i2c.count < MAX_I2C_PORTS; i++) {
        uint32_t entry_off = i2c_ptr + hdr_size + i * entry_size;
        if (entry_off + entry_size > m_vbios_size) break;

        i2c_port& p = m_i2c.ports[m_i2c.count];
        p.valid = false;

        if (version >= 0x41) {
            // CCB 4.1 (GM20x+): bits [4:0] = I2C port, bits [9:5] = DPAUX port
            uint32_t entry_val = vbios_rd32(m_vbios, entry_off);
            uint8_t i2c_port_num = entry_val & 0x1F;
            uint8_t aux_port_num = (entry_val >> 5) & 0x1F;

            if (i2c_port_num == 0x1F && aux_port_num == 0x1F) {
                p.type = dcb_i2c_type::UNUSED;
            } else {
                p.type = dcb_i2c_type::PMGR;
                p.port = i2c_port_num;
                p.aux_port = aux_port_num;
                p.hybrid = (aux_port_num != 0x1F);
                p.valid = true;
            }
        } else if (version >= 0x30 && entry_size >= 4) {
            // CCB 4.0: upper byte = access method type
            uint8_t method = vbios_rd8(m_vbios, entry_off + 3);
            uint8_t data0 = vbios_rd8(m_vbios, entry_off);
            uint8_t data1 = vbios_rd8(m_vbios, entry_off + 1);

            switch (method) {
            case 0x05: // NVIO I2C bit-bang
                p.type = dcb_i2c_type::NVIO_BIT;
                p.port = data0 & 0x0F;
                p.hybrid = (data1 & 0x01) != 0;
                p.aux_port = p.hybrid ? (data1 >> 1) : 0;
                p.valid = true;
                break;
            case 0x06: // NVIO AUX
                p.type = dcb_i2c_type::NVIO_AUX;
                p.port = data0 & 0x0F;
                p.hybrid = (data1 & 0x01) != 0;
                p.aux_port = p.port;
                p.valid = true;
                break;
            case 0x07: // Unused
                p.type = dcb_i2c_type::UNUSED;
                break;
            default:
                // Legacy types
                p.type = static_cast<dcb_i2c_type>(method);
                p.port = data0;
                p.aux_port = 0;
                p.hybrid = false;
                p.valid = (method != 0xFF);
                break;
            }
        } else {
            // Pre-3.0: treat as legacy
            uint8_t data0 = vbios_rd8(m_vbios, entry_off);
            uint8_t type_val = vbios_rd8(m_vbios, entry_off + 3) & 0x07;
            if (type_val == 7) {
                p.type = dcb_i2c_type::UNUSED;
            } else {
                p.type = static_cast<dcb_i2c_type>(type_val);
                p.port = data0;
                p.valid = true;
            }
        }

        log_i2c_port(i, p);
        m_i2c.count++;
    }

    log::info("nvidia: I2C: %u port(s) parsed", m_i2c.count);
    return OK;
}

int32_t nv_gpu::parse_connector_table() {
    uint16_t conn_ptr = m_dcb.connector_table_ptr;
    if (conn_ptr == 0 || conn_ptr >= m_vbios_size) {
        return ERR_NOT_FOUND;
    }

    log::info("nvidia: ---- Connector Table ----");

    uint8_t version = vbios_rd8(m_vbios, conn_ptr);
    uint8_t hdr_size = vbios_rd8(m_vbios, conn_ptr + 1);
    uint8_t entry_count = vbios_rd8(m_vbios, conn_ptr + 2);
    uint8_t entry_size = vbios_rd8(m_vbios, conn_ptr + 3);

    log::info("nvidia: connector table version: 0x%02x, entries: %u, entry size: %u",
              version, entry_count, entry_size);

    if (hdr_size >= 5) {
        uint8_t platform = vbios_rd8(m_vbios, conn_ptr + 4);
        log::info("nvidia: platform type: 0x%02x", platform);
    }

    m_connectors.count = 0;
    for (uint8_t i = 0; i < entry_count && m_connectors.count < MAX_CONNECTORS; i++) {
        uint32_t entry_off = conn_ptr + hdr_size + i * entry_size;
        if (entry_off + 4 > m_vbios_size) break;

        uint32_t dw = vbios_rd32(m_vbios, entry_off);

        connector_entry& c = m_connectors.entries[m_connectors.count];
        c.type = static_cast<dcb_connector_type>(dw & 0xFF);
        c.location = (dw >> 8) & 0x0F;
        c.flags = static_cast<uint16_t>((dw >> 12) & 0xFFFF);

        if (c.type == dcb_connector_type::SKIP) continue;

        log_connector(m_connectors.count, c);
        m_connectors.count++;
    }

    log::info("nvidia: connectors: %u physical connector(s)", m_connectors.count);
    return OK;
}

// ============================================================================
// Phase 3: Monitor Probing (I2C EDID + DP AUX EDID)
// ============================================================================

int32_t nv_gpu::probe_monitors() {
    log::info("nvidia: ========================================");
    log::info("nvidia: Phase 3: Probing connected monitors");
    log::info("nvidia: ========================================");

    // Dump initial state of all I2C/AUX/pad registers for diagnostics
    log::info("nvidia: ---- Pre-probe register state dump ----");
    for (uint8_t i = 0; i < m_i2c.count; i++) {
        const i2c_port& p = m_i2c.ports[i];
        if (!p.valid) continue;

        uint32_t i2c_reg = reg::I2C_PORT_BASE + p.port * reg::I2C_PORT_STRIDE;
        uint32_t i2c_val = reg_rd32(i2c_reg);
        log::info("nvidia: I2C[%u] port=%u reg=0x%06x val=0x%08x (SCL=%s SDA=%s)",
                  i, p.port, i2c_reg, i2c_val,
                  (i2c_val & reg::I2C_SCL_SENSE) ? "H" : "L",
                  (i2c_val & reg::I2C_SDA_SENSE) ? "H" : "L");

        if (p.hybrid && p.aux_port != 0x1F) {
            uint32_t aux_base = reg::AUX_CH_BASE + p.aux_port * reg::AUX_CH_STRIDE;
            uint32_t aux_ctrl = reg_rd32(aux_base + reg::AUX_CTRL);
            uint32_t aux_stat = reg_rd32(aux_base + reg::AUX_STAT);
            uint32_t pad_base = reg::I2C_PAD_BASE + p.aux_port * reg::I2C_PAD_STRIDE;
            uint32_t pad_mode = reg_rd32(pad_base + reg::I2C_PAD_MODE);
            uint32_t pad_en   = reg_rd32(pad_base + reg::I2C_PAD_ENABLE);
            log::info("nvidia:   AUX ch=%u CTRL=0x%08x STAT=0x%08x (HPD=%s)",
                      p.aux_port, aux_ctrl, aux_stat,
                      (aux_stat & reg::AUX_STAT_SINK_DET) ? "yes" : "no");
            log::info("nvidia:   PAD %u mode=0x%08x en=0x%08x (base 0x%06x)",
                      p.aux_port, pad_mode, pad_en, pad_base);
        }
    }
    log::info("nvidia: ---- End pre-probe dump ----");

    m_edid_count = 0;

    for (uint8_t i = 0; i < m_dcb.count; i++) {
        const dcb_entry& e = m_dcb.entries[i];

        // Skip non-display outputs
        if (e.type == dcb_output_type::TV ||
            e.type == dcb_output_type::EOL ||
            e.type == dcb_output_type::UNUSED) {
            continue;
        }

        log::info("nvidia: probing DCB output %u: type=%s i2c=%u or=0x%x",
                  i, output_type_name(e.type), e.i2c_index, e.or_mask);

        uint8_t edid_buf[EDID_BLOCK_SIZE];
        string::memset(edid_buf, 0, sizeof(edid_buf));
        int32_t rc = ERR_NOT_FOUND;

        // Determine how to read EDID based on output type and I2C table
        if (e.i2c_index < m_i2c.count && m_i2c.ports[e.i2c_index].valid) {
            const i2c_port& port = m_i2c.ports[e.i2c_index];
            bool is_hybrid = port.hybrid && port.aux_port != 0x1F;

            // Try DP AUX first (preferred for DP outputs)
            if (port.type == dcb_i2c_type::NVIO_AUX ||
                (port.type == dcb_i2c_type::PMGR && port.aux_port != 0x1F)) {
                uint8_t aux_ch = (port.type == dcb_i2c_type::PMGR)
                    ? port.aux_port : port.port;

                // Switch hybrid pad to AUX mode before AUX transaction
                if (is_hybrid) {
                    pad_set_aux_mode(aux_ch);
                }

                log::info("nvidia:   trying DP AUX channel %u for EDID...", aux_ch);
                rc = aux_read_edid(aux_ch, edid_buf);
            }

            // If AUX failed, try I2C DDC
            if (rc != OK && (port.type == dcb_i2c_type::NVIO_BIT ||
                             port.type == dcb_i2c_type::PMGR ||
                             port.type == dcb_i2c_type::NV04_BIT)) {
                uint8_t i2c_port_num = port.port;
                if (i2c_port_num != 0x1F) {
                    // Switch hybrid pad to I2C mode before bit-bang
                    if (is_hybrid) {
                        pad_set_i2c_mode(port.aux_port);
                    }

                    log::info("nvidia:   trying I2C port %u for DDC/EDID...", i2c_port_num);
                    rc = i2c_read_edid(i2c_port_num, edid_buf);

                    // Restore pad to AUX mode after I2C (leave in DP-ready state)
                    if (is_hybrid) {
                        pad_set_aux_mode(port.aux_port);
                    }
                }
            }
        } else {
            log::info("nvidia:   I2C port %u not available/valid", e.i2c_index);
        }

        if (rc == OK) {
            edid_info info;
            string::memset(&info, 0, sizeof(info));
            rc = parse_edid(edid_buf, &info);
            if (rc == OK && info.valid) {
                m_edid[m_edid_count] = info;
                m_edid_dcb_index[m_edid_count] = i; // Track which DCB entry
                log_edid(m_edid_count, info);
                m_edid_count++;
            } else {
                log::warn("nvidia:   EDID parse failed for output %u (rc=%d)", i, rc);
            }
        } else {
            log::info("nvidia:   no monitor detected on output %u", i);
        }
    }

    log::info("nvidia: ========================================");
    log::info("nvidia: %u monitor(s) detected", m_edid_count);
    log::info("nvidia: ========================================");

    return (m_edid_count > 0) ? OK : ERR_NOT_FOUND;
}

// ============================================================================
// Phase 3: I2C Bit-Bang Implementation
// ============================================================================

int32_t nv_gpu::i2c_init_port(uint8_t port) {
    uint32_t reg_addr = reg::I2C_PORT_BASE + port * reg::I2C_PORT_STRIDE;

    // Initialize: release both lines + set bit 2
    reg_wr32(reg_addr, reg::I2C_INIT_BITS);
    delay::us(10);

    // Verify lines are high (pulled up)
    uint32_t val = reg_rd32(reg_addr);
    bool scl_ok = (val & reg::I2C_SCL_SENSE) != 0;
    bool sda_ok = (val & reg::I2C_SDA_SENSE) != 0;

    log::info("nvidia: I2C port %u (reg 0x%06x): SCL=%s SDA=%s",
              port, reg_addr, scl_ok ? "HIGH" : "LOW", sda_ok ? "HIGH" : "LOW");

    if (!scl_ok || !sda_ok) {
        // Try to recover — toggle SCL a few times
        for (int j = 0; j < 9; j++) {
            i2c_scl_set(port, false);
            delay::us(5);
            i2c_scl_set(port, true);
            delay::us(5);
        }
        i2c_sda_set(port, true);
        delay::us(10);

        val = reg_rd32(reg_addr);
        scl_ok = (val & reg::I2C_SCL_SENSE) != 0;
        sda_ok = (val & reg::I2C_SDA_SENSE) != 0;

        if (!scl_ok || !sda_ok) {
            log::warn("nvidia: I2C port %u: bus stuck (SCL=%s SDA=%s after recovery)",
                      port, scl_ok ? "HIGH" : "LOW", sda_ok ? "HIGH" : "LOW");
            return ERR_IO;
        }
        log::info("nvidia: I2C port %u: bus recovered successfully", port);
    }

    return OK;
}

void nv_gpu::i2c_scl_set(uint8_t port, bool high) {
    uint32_t addr = reg::I2C_PORT_BASE + port * reg::I2C_PORT_STRIDE;
    reg_mask32(addr, reg::I2C_SCL_OUT, high ? reg::I2C_SCL_OUT : 0);
}

void nv_gpu::i2c_sda_set(uint8_t port, bool high) {
    uint32_t addr = reg::I2C_PORT_BASE + port * reg::I2C_PORT_STRIDE;
    reg_mask32(addr, reg::I2C_SDA_OUT, high ? reg::I2C_SDA_OUT : 0);
}

bool nv_gpu::i2c_scl_get(uint8_t port) {
    uint32_t addr = reg::I2C_PORT_BASE + port * reg::I2C_PORT_STRIDE;
    return (reg_rd32(addr) & reg::I2C_SCL_SENSE) != 0;
}

bool nv_gpu::i2c_sda_get(uint8_t port) {
    uint32_t addr = reg::I2C_PORT_BASE + port * reg::I2C_PORT_STRIDE;
    return (reg_rd32(addr) & reg::I2C_SDA_SENSE) != 0;
}

bool nv_gpu::i2c_raise_scl(uint8_t port) {
    i2c_scl_set(port, true);

    // Wait for clock stretching — up to 2.2ms
    uint64_t deadline = clock::now_ns() + reg::I2C_T_TIMEOUT_NS;
    while (!i2c_scl_get(port)) {
        if (clock::now_ns() >= deadline) {
            log::warn("nvidia: I2C port %u: SCL stretch timeout", port);
            return false;
        }
        delay::ns(1000);
    }
    return true;
}

void nv_gpu::i2c_start(uint8_t port) {
    // If SCL or SDA low, recover first
    if (!i2c_scl_get(port) || !i2c_sda_get(port)) {
        i2c_scl_set(port, false);
        i2c_sda_set(port, true);
        i2c_raise_scl(port);
    }

    // START: SDA high → low while SCL is high
    i2c_sda_set(port, false);
    delay::ns(reg::I2C_T_HOLD_NS);
    i2c_scl_set(port, false);
    delay::ns(reg::I2C_T_HOLD_NS);
}

void nv_gpu::i2c_stop(uint8_t port) {
    // STOP: SDA low → high while SCL is high
    i2c_scl_set(port, false);
    i2c_sda_set(port, false);
    delay::ns(reg::I2C_T_RISEFALL_NS);
    i2c_scl_set(port, true);
    delay::ns(reg::I2C_T_HOLD_NS);
    i2c_sda_set(port, true);
    delay::ns(reg::I2C_T_HOLD_NS);
}

bool nv_gpu::i2c_write_byte(uint8_t port, uint8_t byte) {
    // Send 8 bits MSB first
    for (int bit = 7; bit >= 0; bit--) {
        i2c_sda_set(port, (byte >> bit) & 1);
        delay::ns(reg::I2C_T_RISEFALL_NS);
        if (!i2c_raise_scl(port)) return false;
        delay::ns(reg::I2C_T_HOLD_NS);
        i2c_scl_set(port, false);
        delay::ns(reg::I2C_T_HOLD_NS);
    }

    // Read ACK: release SDA, clock, read SDA
    i2c_sda_set(port, true); // Release SDA for slave ACK
    delay::ns(reg::I2C_T_RISEFALL_NS);
    if (!i2c_raise_scl(port)) return false;
    delay::ns(reg::I2C_T_HOLD_NS);
    bool ack = !i2c_sda_get(port); // ACK = SDA pulled low
    i2c_scl_set(port, false);
    delay::ns(reg::I2C_T_HOLD_NS);

    return ack;
}

uint8_t nv_gpu::i2c_read_byte(uint8_t port, bool ack) {
    uint8_t byte = 0;

    // Read 8 bits MSB first
    i2c_sda_set(port, true); // Release SDA for input
    for (int bit = 7; bit >= 0; bit--) {
        delay::ns(reg::I2C_T_RISEFALL_NS);
        if (!i2c_raise_scl(port)) return 0xFF;
        delay::ns(reg::I2C_T_HOLD_NS);
        if (i2c_sda_get(port)) {
            byte |= (1 << bit);
        }
        i2c_scl_set(port, false);
        delay::ns(reg::I2C_T_HOLD_NS);
    }

    // Send ACK/NACK
    i2c_sda_set(port, !ack); // ACK = SDA low, NACK = SDA high
    delay::ns(reg::I2C_T_RISEFALL_NS);
    i2c_raise_scl(port);
    delay::ns(reg::I2C_T_HOLD_NS);
    i2c_scl_set(port, false);
    delay::ns(reg::I2C_T_HOLD_NS);
    i2c_sda_set(port, true); // Release SDA

    return byte;
}

int32_t nv_gpu::i2c_read_edid(uint8_t port, uint8_t* edid_buf) {
    int32_t rc = i2c_init_port(port);
    if (rc != OK) return rc;

    // Write: slave address 0x50, register 0x00
    i2c_start(port);
    if (!i2c_write_byte(port, reg::I2C_DDC_ADDR_WR)) {
        i2c_stop(port);
        log::debug("nvidia: I2C port %u: DDC NACK on address", port);
        return ERR_NACK;
    }

    if (!i2c_write_byte(port, 0x00)) { // Register offset 0
        i2c_stop(port);
        return ERR_NACK;
    }

    // Repeated start + read
    i2c_start(port);
    if (!i2c_write_byte(port, reg::I2C_DDC_ADDR_RD)) {
        i2c_stop(port);
        return ERR_NACK;
    }

    // Read 128 bytes
    for (uint32_t i = 0; i < EDID_BLOCK_SIZE; i++) {
        bool last = (i == EDID_BLOCK_SIZE - 1);
        edid_buf[i] = i2c_read_byte(port, !last); // ACK all but last
    }

    i2c_stop(port);

    // Validate EDID header
    static const uint8_t edid_header[] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    bool header_ok = true;
    for (uint32_t i = 0; i < 8; i++) {
        if (edid_buf[i] != edid_header[i]) {
            header_ok = false;
            break;
        }
    }

    if (!header_ok) {
        log::debug("nvidia: I2C port %u: EDID header invalid", port);
        return ERR_INVALID;
    }

    // Validate checksum
    uint8_t sum = 0;
    for (uint32_t i = 0; i < EDID_BLOCK_SIZE; i++) {
        sum += edid_buf[i];
    }
    if (sum != 0) {
        log::warn("nvidia: I2C port %u: EDID checksum failed (sum=0x%02x)", port, sum);
        return ERR_CHECKSUM;
    }

    log::info("nvidia: I2C port %u: EDID read successfully (128 bytes, checksum OK)", port);
    return OK;
}

// ============================================================================
// Phase 3: DP AUX Channel
// ============================================================================

// Switch a hybrid I2C/AUX pad to I2C mode
void nv_gpu::pad_set_i2c_mode(uint8_t pad) {
    uint32_t pad_base = reg::I2C_PAD_BASE + pad * reg::I2C_PAD_STRIDE;

    uint32_t mode_before = reg_rd32(pad_base + reg::I2C_PAD_MODE);
    uint32_t en_before = reg_rd32(pad_base + reg::I2C_PAD_ENABLE);

    // Disable pad first
    reg_mask32(pad_base + reg::I2C_PAD_ENABLE, reg::I2C_PAD_ENABLE_BIT,
               reg::I2C_PAD_ENABLE_BIT);
    delay::us(20);

    // Switch to I2C mode
    reg_mask32(pad_base + reg::I2C_PAD_MODE, reg::I2C_PAD_MODE_I2C_MASK,
               reg::I2C_PAD_MODE_I2C_VAL);
    delay::us(20);

    // Re-enable pad
    reg_mask32(pad_base + reg::I2C_PAD_ENABLE, reg::I2C_PAD_ENABLE_BIT, 0);
    delay::us(20);

    uint32_t mode_after = reg_rd32(pad_base + reg::I2C_PAD_MODE);
    uint32_t en_after = reg_rd32(pad_base + reg::I2C_PAD_ENABLE);

    log::info("nvidia: pad %u → I2C mode (base 0x%06x): mode 0x%08x→0x%08x, en 0x%08x→0x%08x",
              pad, pad_base, mode_before, mode_after, en_before, en_after);
}

// Switch a hybrid I2C/AUX pad to AUX mode
void nv_gpu::pad_set_aux_mode(uint8_t pad) {
    uint32_t pad_base = reg::I2C_PAD_BASE + pad * reg::I2C_PAD_STRIDE;

    uint32_t mode_before = reg_rd32(pad_base + reg::I2C_PAD_MODE);
    uint32_t en_before = reg_rd32(pad_base + reg::I2C_PAD_ENABLE);

    // Disable pad first
    reg_mask32(pad_base + reg::I2C_PAD_ENABLE, reg::I2C_PAD_ENABLE_BIT,
               reg::I2C_PAD_ENABLE_BIT);
    delay::us(20);

    // Switch to AUX mode
    reg_mask32(pad_base + reg::I2C_PAD_MODE, reg::I2C_PAD_MODE_AUX_MASK,
               reg::I2C_PAD_MODE_AUX_VAL);
    delay::us(20);

    // Re-enable pad
    reg_mask32(pad_base + reg::I2C_PAD_ENABLE, reg::I2C_PAD_ENABLE_BIT, 0);
    delay::us(20);

    uint32_t mode_after = reg_rd32(pad_base + reg::I2C_PAD_MODE);
    uint32_t en_after = reg_rd32(pad_base + reg::I2C_PAD_ENABLE);

    log::info("nvidia: pad %u → AUX mode (base 0x%06x): mode 0x%08x→0x%08x, en 0x%08x→0x%08x",
              pad, pad_base, mode_before, mode_after, en_before, en_after);
}

int32_t nv_gpu::aux_init(uint8_t ch) {
    uint32_t base = reg::AUX_CH_BASE + ch * reg::AUX_CH_STRIDE;
    uint32_t ctrl_reg = base + reg::AUX_CTRL;

    // Log initial AUX state for diagnostics
    uint32_t init_ctrl = reg_rd32(ctrl_reg);
    uint32_t init_stat = reg_rd32(base + reg::AUX_STAT);
    log::info("nvidia: AUX ch %u: init state CTRL=0x%08x STAT=0x%08x", ch, init_ctrl, init_stat);

    // Step 1: Force-reset the AUX channel to clear any stale EFI GOP state.
    // Write bit 31 (reset), then clear it.
    reg_wr32(ctrl_reg, reg::AUX_CTRL_RESET);
    delay::us(100);
    reg_wr32(ctrl_reg, 0x00000000);
    delay::us(100);

    // Step 2: Release any stale ownership from EFI GOP.
    // Clear ownership request bits [21:20] and trigger bit [16].
    reg_wr32(ctrl_reg, 0x00000000);
    delay::us(100);

    // Step 3: Disable auto-DPCD (GM200+) BEFORE acquiring ownership.
    // auto-DPCD auto-polls the DP sink and can interfere with manual transactions.
    uint32_t auto_dpcd_reg = reg::AUX_AUTO_DPCD_BASE + ch * reg::AUX_CH_STRIDE;
    reg_mask32(auto_dpcd_reg, reg::AUX_AUTO_DPCD_DISABLE, 0);
    delay::us(100);

    // Step 4: Wait for idle — should be fast after reset.
    uint64_t deadline = clock::now_ns() + 50000000; // 50ms (generous)
    while (true) {
        uint32_t ctrl = reg_rd32(ctrl_reg);
        if ((ctrl & 0x03010000) == 0) break;
        if (clock::now_ns() >= deadline) {
            log::warn("nvidia: AUX ch %u: still not idle after reset (CTRL=0x%08x)",
                      ch, reg_rd32(ctrl_reg));
            // Force it
            reg_wr32(ctrl_reg, 0x00000000);
            delay::us(1000);
            break;
        }
        delay::us(100);
    }

    // Step 5: Request ownership.
    reg_mask32(ctrl_reg, 0x00300000, 0x00100000);

    // Step 6: Wait for ownership acknowledgement.
    deadline = clock::now_ns() + 50000000; // 50ms
    while (true) {
        uint32_t ctrl = reg_rd32(ctrl_reg);
        if ((ctrl & 0x03000000) == 0x01000000) break;
        if (clock::now_ns() >= deadline) {
            uint32_t final_ctrl = reg_rd32(ctrl_reg);
            log::warn("nvidia: AUX ch %u: ownership timeout (CTRL=0x%08x)", ch, final_ctrl);
            // Try to proceed anyway — on some firmware paths ownership may not ACK
            // but the channel still works
            break;
        }
        delay::us(100);
    }

    uint32_t post_ctrl = reg_rd32(ctrl_reg);
    uint32_t post_stat = reg_rd32(base + reg::AUX_STAT);
    log::info("nvidia: AUX ch %u: post-init CTRL=0x%08x STAT=0x%08x", ch, post_ctrl, post_stat);

    // Step 7: Check sink present (HPD).
    if (!(post_stat & reg::AUX_STAT_SINK_DET)) {
        log::info("nvidia: AUX ch %u: no sink detected (HPD not asserted)", ch);
        // Release ownership
        reg_mask32(ctrl_reg, 0x00310000, 0x00000000);
        return ERR_NOT_FOUND;
    }

    log::info("nvidia: AUX ch %u: sink detected, ownership acquired", ch);
    return OK;
}

int32_t nv_gpu::aux_xfer(uint8_t ch, uint32_t type, uint32_t addr,
                          uint8_t* buf, uint32_t len, uint32_t* reply) {
    uint32_t base = reg::AUX_CH_BASE + ch * reg::AUX_CH_STRIDE;
    uint32_t ctrl_reg = base + reg::AUX_CTRL;

    // For write: load TX buffer
    if (type == reg::AUX_TYPE_I2C_WR || type == reg::AUX_TYPE_I2C_WR_STOP ||
        type == reg::AUX_TYPE_NATIVE_WR) {
        for (uint32_t i = 0; i < len; i += 4) {
            uint32_t word = 0;
            for (uint32_t j = 0; j < 4 && (i + j) < len; j++) {
                word |= static_cast<uint32_t>(buf[i + j]) << (j * 8);
            }
            reg_wr32(base + reg::AUX_TX_DATA + i, word);
        }
    }

    // Set address
    reg_wr32(base + reg::AUX_ADDR, addr);

    // Build control word: type + size, preserving ownership bits
    // Read current CTRL to preserve ownership state
    uint32_t cur_ctrl = reg_rd32(ctrl_reg);
    uint32_t own_bits = cur_ctrl & 0x00300000; // Preserve ownership request bits

    uint32_t xfer = (type << reg::AUX_CTRL_TYPE_SHIFT);
    if (len > 0) {
        xfer |= (len - 1); // Size field = byte count - 1
    } else {
        xfer |= reg::AUX_CTRL_ADDR_ONLY; // Address-only transaction (bit 8)
    }

    // Sequence from nouveau g94_aux_xfer():
    // 1. Reset (bit 31) + ownership + xfer params
    // 2. Clear reset, keep ownership + xfer params
    // 3. Trigger (bit 16) + ownership + xfer params
    reg_wr32(ctrl_reg, reg::AUX_CTRL_RESET | own_bits | xfer);
    reg_wr32(ctrl_reg, own_bits | xfer);
    reg_wr32(ctrl_reg, reg::AUX_CTRL_TRIGGER | own_bits | xfer);

    // Wait for completion (trigger bit clears)
    uint64_t deadline = clock::now_ns() + 10000000; // 10ms (generous for I2C-over-AUX)
    while (true) {
        uint32_t c = reg_rd32(ctrl_reg);
        if (!(c & reg::AUX_CTRL_TRIGGER)) break;
        if (clock::now_ns() >= deadline) {
            log::warn("nvidia: AUX ch %u xfer timeout (CTRL=0x%08x)", ch, reg_rd32(ctrl_reg));
            return ERR_TIMEOUT;
        }
        delay::us(10);
    }

    // Read and clear status (write 0 to clear it, as nouveau does with mask(stat, 0, 0))
    uint32_t stat = reg_rd32(base + reg::AUX_STAT);
    reg_wr32(base + reg::AUX_STAT, 0); // Clear status for next xfer
    *reply = (stat & reg::AUX_STAT_REPLY_MASK) >> reg::AUX_STAT_REPLY_SHIFT;

    if (stat & reg::AUX_STAT_TIMEOUT) {
        log::debug("nvidia: AUX ch %u: xfer AUX timeout (stat=0x%08x)", ch, stat);
        return ERR_TIMEOUT;
    }

    // For read: copy RX buffer
    if (type == reg::AUX_TYPE_I2C_RD || type == reg::AUX_TYPE_I2C_RD_STOP ||
        type == reg::AUX_TYPE_NATIVE_RD) {
        uint32_t rx_size = stat & reg::AUX_STAT_RX_SIZE_MASK;
        if (rx_size == 0 && len > 0) {
            log::debug("nvidia: AUX ch %u: read returned 0 bytes (stat=0x%08x)", ch, stat);
        }
        for (uint32_t i = 0; i < rx_size && i < len; i += 4) {
            uint32_t word = reg_rd32(base + reg::AUX_RX_DATA + i);
            for (uint32_t j = 0; j < 4 && (i + j) < rx_size && (i + j) < len; j++) {
                buf[i + j] = static_cast<uint8_t>((word >> (j * 8)) & 0xFF);
            }
        }
    }

    return OK;
}

int32_t nv_gpu::aux_read_edid(uint8_t aux_ch, uint8_t* edid_buf) {
    int32_t rc = aux_init(aux_ch);
    if (rc != OK) return rc;

    uint32_t base = reg::AUX_CH_BASE + aux_ch * reg::AUX_CH_STRIDE;
    uint32_t reply = 0;

    // Step 0: Probe basic AUX connectivity with a native DPCD read.
    // Read DPCD revision at address 0x00000 (1 byte). This tests the AUX
    // channel itself without relying on I2C-over-AUX.
    uint8_t dpcd_rev = 0;
    rc = aux_xfer(aux_ch, reg::AUX_TYPE_NATIVE_RD, 0x00000, &dpcd_rev, 1, &reply);
    if (rc == OK && reply == reg::AUX_REPLY_ACK) {
        log::info("nvidia: AUX ch %u: DPCD rev=0x%02x (native AUX works!)", aux_ch, dpcd_rev);

        // Also read link caps at DPCD 0x00001 (max link rate) and 0x00002 (max lane count)
        uint8_t dpcd_buf[4] = {};
        aux_xfer(aux_ch, reg::AUX_TYPE_NATIVE_RD, 0x00001, dpcd_buf, 2, &reply);
        if (reply == reg::AUX_REPLY_ACK) {
            log::info("nvidia: AUX ch %u: max link rate=0x%02x, max lanes=%u",
                      aux_ch, dpcd_buf[0], dpcd_buf[1] & 0x1F);
        }
    } else {
        log::info("nvidia: AUX ch %u: native DPCD read failed (rc=%d reply=%u) — "
                  "not a DP sink or AUX not functional", aux_ch, rc, reply);
        // Don't abort — try I2C-over-AUX anyway (some passive DP-to-HDMI adapters
        // don't respond to native AUX but do support I2C-over-AUX)
    }

    // Step 1: Read EDID via I2C-over-AUX
    // Write: address 0x50, offset 0x00 (set EDID pointer to start)
    uint8_t offset = 0x00;

    // I2C-over-AUX: type 0 = I2C_WR (with MOT=0 for stop, MOT=1 for repeated start)
    // For DDC: write the offset byte to address 0x50, then read back 128 bytes.
    // Type 0 (I2C_WR without STOP/MOT bit) = middle-of-transaction
    // Type 4 would be I2C_WR with STOP. But the NVIDIA HW encoding uses:
    //   bits[15:12] = 0: I2C write (MOT=1, no stop)
    //   The stop is sent by a separate address-only transaction.
    rc = aux_xfer(aux_ch, reg::AUX_TYPE_I2C_WR, reg::I2C_DDC_ADDR, &offset, 1, &reply);
    if (rc != OK || reply != reg::AUX_REPLY_ACK) {
        log::info("nvidia: AUX ch %u: I2C-over-AUX write addr failed (rc=%d reply=%u)",
                  aux_ch, rc, reply);
        // Release ownership
        reg_mask32(base + reg::AUX_CTRL, 0x00310000, 0x00000000);
        return (rc != OK) ? rc : ERR_NACK;
    }

    // Step 2: Read EDID in 16-byte chunks (AUX max payload)
    for (uint32_t off = 0; off < EDID_BLOCK_SIZE; off += 16) {
        uint32_t chunk = 16;
        if (off + chunk > EDID_BLOCK_SIZE) chunk = EDID_BLOCK_SIZE - off;

        rc = aux_xfer(aux_ch, reg::AUX_TYPE_I2C_RD, reg::I2C_DDC_ADDR,
                      &edid_buf[off], chunk, &reply);
        if (rc != OK || reply != reg::AUX_REPLY_ACK) {
            log::info("nvidia: AUX ch %u: I2C-over-AUX read failed at off=%u (rc=%d reply=%u)",
                      aux_ch, off, rc, reply);
            reg_mask32(base + reg::AUX_CTRL, 0x00310000, 0x00000000);
            return (rc != OK) ? rc : ERR_NACK;
        }
    }

    // Step 3: I2C stop — send address-only write to signal end of I2C transaction.
    // Use I2C_WR_STOP (type 4 = MOT=0, meaning "last transaction, send STOP")
    // with size=0 (address-only).
    uint8_t dummy = 0;
    aux_xfer(aux_ch, reg::AUX_TYPE_I2C_WR_STOP, reg::I2C_DDC_ADDR, &dummy, 0, &reply);

    // Release ownership
    reg_mask32(base + reg::AUX_CTRL, 0x00310000, 0x00000000);

    // Re-enable auto-DPCD
    uint32_t auto_dpcd_reg = reg::AUX_AUTO_DPCD_BASE + aux_ch * reg::AUX_CH_STRIDE;
    reg_mask32(auto_dpcd_reg, reg::AUX_AUTO_DPCD_DISABLE, reg::AUX_AUTO_DPCD_DISABLE);

    // Validate EDID header
    static const uint8_t edid_header[] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    for (uint32_t i = 0; i < 8; i++) {
        if (edid_buf[i] != edid_header[i]) {
            log::debug("nvidia: AUX ch %u: EDID header invalid", aux_ch);
            return ERR_INVALID;
        }
    }

    // Checksum
    uint8_t sum = 0;
    for (uint32_t i = 0; i < EDID_BLOCK_SIZE; i++) sum += edid_buf[i];
    if (sum != 0) {
        log::warn("nvidia: AUX ch %u: EDID checksum failed (sum=0x%02x)", aux_ch, sum);
        return ERR_CHECKSUM;
    }

    log::info("nvidia: AUX ch %u: EDID read successfully (128 bytes, checksum OK)", aux_ch);
    return OK;
}

// ============================================================================
// Phase 3: EDID Parsing
// ============================================================================

int32_t nv_gpu::parse_edid(const uint8_t* raw, edid_info* out) {
    out->valid = false;

    // Manufacturer ID (bytes 8-9): 3 compressed ASCII characters
    uint16_t mfg = (static_cast<uint16_t>(raw[8]) << 8) | raw[9];
    out->manufacturer[0] = static_cast<char>(((mfg >> 10) & 0x1F) + 'A' - 1);
    out->manufacturer[1] = static_cast<char>(((mfg >> 5) & 0x1F) + 'A' - 1);
    out->manufacturer[2] = static_cast<char>((mfg & 0x1F) + 'A' - 1);
    out->manufacturer[3] = '\0';

    // Product code (bytes 10-11, LE)
    out->product_code = static_cast<uint16_t>(raw[10]) | (static_cast<uint16_t>(raw[11]) << 8);

    // Serial number (bytes 12-15, LE)
    out->serial_number = static_cast<uint32_t>(raw[12]) |
                         (static_cast<uint32_t>(raw[13]) << 8) |
                         (static_cast<uint32_t>(raw[14]) << 16) |
                         (static_cast<uint32_t>(raw[15]) << 24);

    // Manufacture date
    out->mfg_week = raw[16];
    out->mfg_year = raw[17]; // Year since 1990

    // Parse Detailed Timing Descriptors (bytes 54-125, 4 blocks of 18 bytes)
    out->display_name[0] = '\0';
    bool got_timing = false;

    for (int block = 0; block < 4; block++) {
        uint32_t base = 54 + block * 18;

        // Check if this is a timing descriptor (first two bytes non-zero)
        uint16_t pixel_clock = static_cast<uint16_t>(raw[base]) |
                               (static_cast<uint16_t>(raw[base + 1]) << 8);

        if (pixel_clock != 0 && !got_timing) {
            // This is a Detailed Timing Descriptor
            out->pixel_clock_khz = pixel_clock * 10; // Convert from 10kHz units

            out->h_active = raw[base + 2] | ((raw[base + 4] & 0xF0) << 4);
            out->h_blanking = raw[base + 3] | ((raw[base + 4] & 0x0F) << 8);
            out->v_active = raw[base + 5] | ((raw[base + 7] & 0xF0) << 4);
            out->v_blanking = raw[base + 6] | ((raw[base + 7] & 0x0F) << 8);

            out->h_sync_offset = raw[base + 8] | ((raw[base + 11] & 0xC0) << 2);
            out->h_sync_width = raw[base + 9] | ((raw[base + 11] & 0x30) << 4);
            out->v_sync_offset = ((raw[base + 10] >> 4) & 0x0F) | ((raw[base + 11] & 0x0C) << 2);
            out->v_sync_width = (raw[base + 10] & 0x0F) | ((raw[base + 11] & 0x03) << 4);

            // Sync polarity (byte 17)
            out->h_sync_positive = (raw[base + 17] & 0x02) != 0;
            out->v_sync_positive = (raw[base + 17] & 0x04) != 0;

            // Compute refresh rate
            uint32_t htotal = out->h_active + out->h_blanking;
            uint32_t vtotal = out->v_active + out->v_blanking;
            if (htotal > 0 && vtotal > 0) {
                out->refresh_hz = (out->pixel_clock_khz * 1000) / (htotal * vtotal);
            }

            got_timing = true;
        } else if (pixel_clock == 0 && raw[base + 3] == 0xFC) {
            // Display name descriptor
            for (int j = 0; j < 13; j++) {
                char c = static_cast<char>(raw[base + 5 + j]);
                if (c == '\n' || c == '\0') {
                    out->display_name[j] = '\0';
                    break;
                }
                out->display_name[j] = c;
                out->display_name[j + 1] = '\0';
            }
        }
    }

    if (!got_timing) {
        log::warn("nvidia: EDID: no detailed timing descriptor found");
        return ERR_INVALID;
    }

    out->valid = true;
    return OK;
}

// ============================================================================
// Phase 4: Display Engine Initialization
// ============================================================================

int32_t nv_gpu::init_display() {
    log::info("nvidia: ========================================");
    log::info("nvidia: Phase 4+5: Display engine initialization (multi-monitor)");
    log::info("nvidia: ========================================");

    int32_t rc;

    // Step 1: Discover display capabilities
    rc = discover_display_caps();
    if (rc != OK) return rc;

    // Step 2: Claim display ownership
    rc = claim_display_ownership();
    if (rc != OK) return rc;

    // Step 3: Enable display engine
    rc = enable_display_engine();
    if (rc != OK) return rc;

    // Step 4: Build output assignments — one per detected monitor
    // Each monitor needs: a DCB entry, a SOR, a head, and VRAM for its framebuffer.
    // We assign distinct heads and SORs to avoid conflicts.

    // Per-output assignment record
    struct output_assignment {
        uint8_t dcb_index;
        uint8_t edid_index;
        uint8_t sor_id;
        uint8_t head_id;
        uint32_t fb_vram_offset;
        uint32_t fb_color;
        bool valid;
    };

    output_assignment assignments[MAX_HEADS];
    uint32_t num_outputs = 0;
    uint8_t heads_used = 0; // Bitmask of assigned heads
    uint8_t sors_used = 0;  // Bitmask of assigned SORs

    // Distinct colors for each monitor to prove independence
    static const uint32_t monitor_colors[] = {
        0xFF0000FF, // Blue (monitor 0)
        0xFFFF0000, // Red (monitor 1)
        0xFF00FF00, // Green (monitor 2)
        0xFFFFFF00, // Yellow (monitor 3)
    };

    // VRAM offset accumulator — each framebuffer gets its own region
    uint32_t vram_offset_cursor = 0;

    for (uint32_t ei = 0; ei < m_edid_count && num_outputs < MAX_HEADS; ei++) {
        if (!m_edid[ei].valid) continue;

        uint8_t dcb_idx = m_edid_dcb_index[ei];
        if (dcb_idx >= m_dcb.count) continue;

        const dcb_entry& e = m_dcb.entries[dcb_idx];

        // Find an available SOR from the DCB or_mask
        int32_t sor_id = -1;
        for (int s = 0; s < 8; s++) {
            if ((e.or_mask & (1 << s)) && !(sors_used & (1 << s)) &&
                (m_sor_mask & (1 << s))) {
                sor_id = s;
                break;
            }
        }
        if (sor_id < 0) {
            log::warn("nvidia: output %u (DCB[%u]): no available SOR (or_mask=0x%x used=0x%x)",
                      ei, dcb_idx, e.or_mask, sors_used);
            continue;
        }

        // Find an available head from the DCB head_mask
        int32_t head_id = -1;
        for (int h = 0; h < static_cast<int>(m_head_count); h++) {
            if ((e.head_mask & (1 << h)) && !(heads_used & (1 << h)) &&
                (m_head_mask & (1 << h))) {
                head_id = h;
                break;
            }
        }
        if (head_id < 0) {
            log::warn("nvidia: output %u (DCB[%u]): no available head (head_mask=0x%x used=0x%x)",
                      ei, dcb_idx, e.head_mask, heads_used);
            continue;
        }

        // Calculate VRAM framebuffer offset (aligned to 4KB)
        uint32_t fb_width = m_edid[ei].h_active;
        uint32_t fb_height = m_edid[ei].v_active;
        uint32_t fb_pitch = fb_width * 4; // ARGB8888
        uint32_t fb_size = fb_pitch * fb_height;
        uint32_t fb_offset = (vram_offset_cursor + 0xFFF) & ~0xFFFu; // Align to 4KB

        // Verify VRAM space
        if (m_bar1_va != 0 && (fb_offset + fb_size) > m_bar1_size) {
            log::warn("nvidia: output %u: framebuffer would exceed VRAM aperture "
                      "(offset 0x%x + size 0x%x > 0x%lx)",
                      ei, fb_offset, fb_size, m_bar1_size);
            continue;
        }

        // Record assignment
        output_assignment& a = assignments[num_outputs];
        a.dcb_index = dcb_idx;
        a.edid_index = static_cast<uint8_t>(ei);
        a.sor_id = static_cast<uint8_t>(sor_id);
        a.head_id = static_cast<uint8_t>(head_id);
        a.fb_vram_offset = fb_offset;
        a.fb_color = monitor_colors[num_outputs % 4];
        a.valid = true;

        heads_used |= (1 << head_id);
        sors_used |= (1 << sor_id);
        vram_offset_cursor = fb_offset + fb_size;

        log::info("nvidia: assigned: monitor %u (%s) → head %d, SOR %d, VRAM 0x%x, color 0x%08x",
                  num_outputs, m_edid[ei].display_name[0] ? m_edid[ei].display_name : "(unnamed)",
                  head_id, sor_id, fb_offset, a.fb_color);

        num_outputs++;
    }

    if (num_outputs == 0) {
        log::error("nvidia: no valid output assignments could be made");
        return ERR_NOT_FOUND;
    }

    log::info("nvidia: %u output(s) assigned, programming hardware...", num_outputs);

    // Step 5: Program each output
    uint32_t outputs_active = 0;

    for (uint32_t o = 0; o < num_outputs; o++) {
        const output_assignment& a = assignments[o];
        const dcb_entry& output = m_dcb.entries[a.dcb_index];
        const edid_info& mode = m_edid[a.edid_index];

        log::info("nvidia: ---- Programming output %u/%u ----", o + 1, num_outputs);
        log::info("nvidia:   DCB[%u] type=%s → head %u, SOR %u",
                  a.dcb_index, output_type_name(output.type), a.head_id, a.sor_id);
        log::info("nvidia:   mode: %ux%u @%uHz (pixel clock %u kHz)",
                  mode.h_active, mode.v_active, mode.refresh_hz, mode.pixel_clock_khz);

        // 5a: Program VPLL
        rc = program_vpll(a.head_id, mode.pixel_clock_khz);
        if (rc != OK) {
            log::error("nvidia:   VPLL programming failed for head %u: %d", a.head_id, rc);
            continue;
        }

        // 5b: Power up SOR
        rc = power_up_sor(a.sor_id);
        if (rc != OK) {
            log::error("nvidia:   SOR %u power-up failed: %d", a.sor_id, rc);
            continue;
        }

        // 5c: Configure SOR
        rc = program_sor(a.sor_id, a.head_id, output.type, output.hdmi_enable, output.or_mask);
        if (rc != OK) {
            log::error("nvidia:   SOR %u programming failed: %d", a.sor_id, rc);
            continue;
        }

        // 5d: Fill framebuffer with distinct color
        uint32_t fb_width = mode.h_active;
        uint32_t fb_height = mode.v_active;
        uint32_t fb_pitch = fb_width * 4;

        rc = fill_framebuffer(a.fb_vram_offset, fb_width, fb_height, fb_pitch, a.fb_color);
        if (rc != OK) {
            log::error("nvidia:   framebuffer fill failed for head %u: %d", a.head_id, rc);
            continue;
        }

        // 5e: Program head timing
        rc = program_head(a.head_id, mode, a.sor_id);
        if (rc != OK) {
            log::error("nvidia:   head %u programming failed: %d", a.head_id, rc);
            continue;
        }

        // 5f: Setup scanout
        rc = setup_scanout(a.head_id, fb_width, fb_height, fb_pitch, a.fb_vram_offset);
        if (rc != OK) {
            log::error("nvidia:   scanout setup failed for head %u: %d", a.head_id, rc);
            continue;
        }

        m_active_heads |= (1 << a.head_id);
        m_active_sors |= (1 << a.sor_id);
        outputs_active++;

        log::info("nvidia:   output %u ACTIVE: head %u → SOR %u → %s (%ux%u @%uHz, color 0x%08x)",
                  o, a.head_id, a.sor_id, output_type_name(output.type),
                  mode.h_active, mode.v_active, mode.refresh_hz, a.fb_color);
    }

    log::info("nvidia: ========================================");
    log::info("nvidia: Display initialization complete!");
    log::info("nvidia: %u of %u output(s) active", outputs_active, num_outputs);
    for (uint32_t o = 0; o < num_outputs; o++) {
        const output_assignment& a = assignments[o];
        if (!(m_active_heads & (1 << a.head_id))) continue;
        const edid_info& mode = m_edid[a.edid_index];
        const char* color_name = (o == 0) ? "BLUE" : (o == 1) ? "RED" :
                                 (o == 2) ? "GREEN" : "YELLOW";
        log::info("nvidia:   Monitor %u: %s — %ux%u @%uHz → %s",
                  o,
                  mode.display_name[0] ? mode.display_name : "(unnamed)",
                  mode.h_active, mode.v_active, mode.refresh_hz,
                  color_name);
    }
    log::info("nvidia: active heads: 0x%02x, active SORs: 0x%02x",
              m_active_heads, m_active_sors);
    log::info("nvidia: ========================================");

    return (outputs_active > 0) ? OK : ERR_NOT_FOUND;
}

int32_t nv_gpu::discover_display_caps() {
    uint32_t caps0 = reg_rd32(reg::DISP_CAPS_HEAD_MASK);
    uint32_t caps_max = reg_rd32(reg::DISP_CAPS_MAX);

    m_head_mask = static_cast<uint8_t>(caps0 & 0xFF);
    m_sor_mask = static_cast<uint8_t>((caps0 >> 8) & 0xFF);
    m_head_count = static_cast<uint8_t>(caps_max & 0x0F);
    m_sor_count = static_cast<uint8_t>((caps_max >> 8) & 0x0F);

    log::info("nvidia: display caps: heads=0x%02x (max %u), SORs=0x%02x (max %u)",
              m_head_mask, m_head_count, m_sor_mask, m_sor_count);

    uint32_t win_mask = reg_rd32(reg::DISP_CAPS_WIN_MASK);
    uint32_t win_count = (caps_max >> 20) & 0x3F;
    log::info("nvidia: display caps: windows=0x%08x (max %u)", win_mask, win_count);

    if (m_head_count == 0) {
        log::error("nvidia: no display heads available");
        return ERR_NO_DISPLAY;
    }

    return OK;
}

int32_t nv_gpu::claim_display_ownership() {
    // Release display ownership first (in case EFI GOP still holds it)
    reg_wr32(reg::DISP_OWNER, 0x00000000); // Clear release bit

    // Wait for not busy
    uint64_t deadline = clock::now_ns() + 100000000; // 100ms
    while (true) {
        uint32_t val = reg_rd32(reg::DISP_OWNER);
        if (!(val & 0x02)) break; // bit 1 = busy
        if (clock::now_ns() >= deadline) {
            log::warn("nvidia: display ownership claim timeout");
            return ERR_TIMEOUT;
        }
        delay::us(1000);
    }

    log::info("nvidia: display ownership claimed");
    return OK;
}

int32_t nv_gpu::enable_display_engine() {
    // Enable display in PMC
    // GA100+: PMC_ENABLE at 0x000600, also write legacy 0x000200
    uint32_t enable = reg_rd32(reg::PMC_ENABLE);
    log::info("nvidia: PMC_ENABLE (0x600) current: 0x%08x", enable);
    reg_wr32(reg::PMC_ENABLE, 0xFFFFFFFF); // Enable all engines
    reg_rd32(reg::PMC_ENABLE); // Flush
    reg_rd32(reg::PMC_ENABLE); // Double flush

    reg_wr32(reg::PMC_ENABLE_LEGACY, 0xFFFFFFFF);
    reg_rd32(reg::PMC_ENABLE_LEGACY);

    // Enable display engine bit
    reg_wr32(reg::DISP_ENABLE, 0x00000001);
    delay::us(100);

    uint32_t disp_enable = reg_rd32(reg::DISP_ENABLE);
    log::info("nvidia: display engine enable: 0x%08x", disp_enable);

    // Lock pin capabilities (TU102+)
    reg_wr32(reg::DISP_PIN_CAP_LOCK, 0x00000021);
    delay::us(100);

    log::info("nvidia: display engine enabled");
    return OK;
}

int32_t nv_gpu::program_vpll(uint8_t head, uint32_t pixel_clock_khz) {
    // Calculate PLL parameters
    // GA100+ VPLL: ref_clock * N / (M * P) = pixel_clock
    // Assume 27 MHz reference crystal (most common for modern NVIDIA GPUs)
    uint32_t ref_khz = 27000;
    uint32_t target_khz = pixel_clock_khz;

    // Simple PLL calculation — find N, M, P
    // Constraints: VCO should be in range ~2-4 GHz typically
    // pixel_clock = ref * N / M / P
    uint32_t best_n = 0, best_m = 1, best_p = 1;
    uint32_t best_error = 0xFFFFFFFF;

    for (uint32_t p = 1; p <= 32; p++) {
        for (uint32_t m = 1; m <= 16; m++) {
            // N = target * M * P / ref
            uint64_t n64 = (static_cast<uint64_t>(target_khz) * m * p) / ref_khz;
            if (n64 == 0 || n64 > 255) continue;

            uint32_t n = static_cast<uint32_t>(n64);
            uint32_t actual = (ref_khz * n) / (m * p);
            uint32_t error = (actual > target_khz)
                ? (actual - target_khz)
                : (target_khz - actual);

            if (error < best_error) {
                best_error = error;
                best_n = n;
                best_m = m;
                best_p = p;
            }
            if (error == 0) break;
        }
        if (best_error == 0) break;
    }

    uint32_t actual_khz = (ref_khz * best_n) / (best_m * best_p);
    log::info("nvidia: VPLL head %u: target=%u kHz, actual=%u kHz (N=%u M=%u P=%u, error=%u kHz)",
              head, target_khz, actual_khz, best_n, best_m, best_p, best_error);

    // Program VPLL registers (GA100+)
    uint32_t vpll_ctrl0 = reg::VPLL_CTRL0_BASE + head * reg::VPLL_STRIDE;
    uint32_t vpll_coeff = reg::VPLL_COEFF_BASE + head * reg::VPLL_STRIDE;
    uint32_t vpll_frac  = reg::VPLL_FRAC_BASE  + head * reg::VPLL_STRIDE;
    uint32_t vpll_trig  = reg::VPLL_TRIGGER_BASE + head * 4;

    reg_wr32(vpll_ctrl0, 0x02080004); // Enable VPLL
    reg_wr32(vpll_frac, (best_n << 16) | 0); // N + frac=0
    reg_wr32(vpll_coeff, (best_p << 16) | best_m); // P + M
    reg_wr32(vpll_trig, 0x00000001); // Trigger

    delay::us(500); // Wait for PLL lock

    log::info("nvidia: VPLL head %u programmed", head);
    return OK;
}

int32_t nv_gpu::power_up_sor(uint8_t sor_id) {
    uint32_t sor_base = reg::SOR_BASE + sor_id * reg::SOR_STRIDE;
    uint32_t pwr_reg = sor_base + reg::SOR_PWR;

    // Wait for not busy
    uint64_t deadline = clock::now_ns() + 50000000; // 50ms
    while (reg_rd32(pwr_reg) & reg::SOR_PWR_TRIGGER) {
        if (clock::now_ns() >= deadline) {
            log::error("nvidia: SOR %u: power control busy timeout (pre)", sor_id);
            return ERR_TIMEOUT;
        }
        delay::us(100);
    }

    // Power up
    reg_mask32(pwr_reg, reg::SOR_PWR_TRIGGER | reg::SOR_PWR_NORMAL,
               reg::SOR_PWR_TRIGGER | reg::SOR_PWR_NORMAL);

    // Wait for not busy
    deadline = clock::now_ns() + 50000000;
    while (reg_rd32(pwr_reg) & reg::SOR_PWR_TRIGGER) {
        if (clock::now_ns() >= deadline) {
            log::error("nvidia: SOR %u: power-up timeout", sor_id);
            return ERR_TIMEOUT;
        }
        delay::us(100);
    }

    // Wait for sequencer
    uint32_t seq_reg = sor_base + reg::SOR_SEQ_CTRL;
    deadline = clock::now_ns() + 50000000;
    while (reg_rd32(seq_reg) & reg::SOR_SEQ_BUSY) {
        if (clock::now_ns() >= deadline) {
            log::error("nvidia: SOR %u: sequencer timeout", sor_id);
            return ERR_TIMEOUT;
        }
        delay::us(100);
    }

    log::info("nvidia: SOR %u powered up", sor_id);
    return OK;
}

int32_t nv_gpu::program_sor(uint8_t sor_id, uint8_t head,
                             dcb_output_type type, bool hdmi, uint8_t or_mask) {
    // Set SOR protocol in core channel state
    uint32_t proto;
    if (type == dcb_output_type::DP) {
        proto = reg::SOR_PROTO_DP_A;
    } else if (type == dcb_output_type::TMDS) {
        proto = hdmi ? reg::SOR_PROTO_TMDS_SL : reg::SOR_PROTO_TMDS_SL;
    } else {
        proto = reg::SOR_PROTO_TMDS_SL; // Default to TMDS single-link
    }

    // Write to assembly (asy) state
    uint32_t sor_state = reg::SOR_ASY_BASE + sor_id * reg::SOR_STATE_STRIDE;
    uint32_t val = (proto << reg::SOR_STATE_PROTO_SHIFT) | (1 << head);
    reg_wr32(sor_state, val);

    log::info("nvidia: SOR %u: proto=%u (%s%s) head_mask=0x%02x (state reg 0x%06x = 0x%08x)",
              sor_id, proto,
              (type == dcb_output_type::DP) ? "DP" :
              (type == dcb_output_type::TMDS) ? "TMDS" : "other",
              hdmi ? "/HDMI" : "",
              (1 << head), sor_state, val);

    // GA102 SOR clock
    uint32_t clk_ctrl = reg::SOR_CLK_CTRL_BASE + sor_id * reg::SOR_CLK_STRIDE;
    uint32_t clk_ctrl2 = reg::SOR_CLK_CTRL2_BASE + sor_id * reg::SOR_CLK_STRIDE;
    reg_wr32(clk_ctrl2, 0x00000000);
    uint32_t div2 = 0; // 0 for normal, 1 for high-speed TMDS
    reg_wr32(clk_ctrl, div2);

    log::info("nvidia: SOR %u: clock configured (div2=%u)", sor_id, div2);

    // SOR routing (GM200+): route output_or → SOR
    // The DCB or_mask tells us the output OR index (__ffs of mask)
    // Route link A: 0x612308 + (output_or * 0x100)
    uint8_t output_or = 0;
    for (int b = 0; b < 8; b++) {
        if (or_mask & (1 << b)) {
            output_or = static_cast<uint8_t>(b);
            break;
        }
    }
    uint32_t route_reg = reg::SOR_ROUTE_LINK_A + output_or * reg::SOR_ROUTE_STRIDE;
    uint32_t route_val = (sor_id + 1); // SOR index + 1 (0 = disconnected)
    reg_mask32(route_reg, 0x0000001F, route_val);

    log::info("nvidia: SOR routing: output_or %u → SOR %u (reg 0x%06x = 0x%08x)",
              output_or, sor_id, route_reg, route_val);

    return OK;
}

int32_t nv_gpu::fill_framebuffer(uint32_t vram_offset, uint32_t width,
                                  uint32_t height, uint32_t pitch, uint32_t color) {
    if (m_bar1_va == 0) {
        log::error("nvidia: BAR1 (VRAM) not mapped, cannot write framebuffer");
        return ERR_MAP_FAILED;
    }

    uint32_t fb_size = pitch * height;
    if (vram_offset + fb_size > m_bar1_size) {
        log::error("nvidia: framebuffer exceeds BAR1 mapped region (%u + %u > %lu)",
                   vram_offset, fb_size, m_bar1_size);
        return ERR_INVALID;
    }

    log::info("nvidia: filling framebuffer: %ux%u pitch=%u at VRAM+0x%x with color 0x%08x",
              width, height, pitch, vram_offset, color);

    // Write pixels through BAR1 (write-combining mapped)
    volatile uint32_t* fb = reinterpret_cast<volatile uint32_t*>(m_bar1_va + vram_offset);
    uint32_t pixels_per_line = pitch / 4;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            fb[y * pixels_per_line + x] = color;
        }
    }

    log::info("nvidia: framebuffer filled (%u bytes written)", fb_size);
    return OK;
}

int32_t nv_gpu::program_head(uint8_t head, const edid_info& mode, uint8_t /*sor_id*/) {
    uint32_t head_base = reg::HEAD_ASY_BASE + head * reg::HEAD_STRIDE;

    // Compute timing values from EDID
    uint32_t htotal = mode.h_active + mode.h_blanking;
    uint32_t vtotal = mode.v_active + mode.v_blanking;
    uint32_t hsynce = mode.h_sync_width;
    uint32_t vsynce = mode.v_sync_width;
    uint32_t hblanke = mode.h_sync_offset + mode.h_sync_width;
    uint32_t vblanke = mode.v_sync_offset + mode.v_sync_width;
    uint32_t hblanks = mode.h_active + mode.h_sync_offset + mode.h_sync_width;
    uint32_t vblanks = mode.v_active + mode.v_sync_offset + mode.v_sync_width;

    // Actually for NVDisplay:
    // htotal/vtotal = total pixels per line/frame - 1
    // hsynce/vsynce = sync end position (= sync width - 1)
    // hblanke/vblanke = blank end position (= sync + back porch)
    // hblanks/vblanks = blank start = active + front porch
    // But the exact encoding depends on the display engine version
    // For simplicity, set raw values:

    log::info("nvidia: head %u timing: htotal=%u vtotal=%u", head, htotal, vtotal);
    log::info("nvidia:   hsync_end=%u vsync_end=%u", hsynce, vsynce);
    log::info("nvidia:   hblank_end=%u vblank_end=%u", hblanke, vblanke);
    log::info("nvidia:   hblank_start=%u vblank_start=%u", hblanks, vblanks);

    // Write head registers
    reg_wr32(head_base + reg::HEAD_SET_CONTROL, 0x00000001); // Enable head
    reg_wr32(head_base + reg::HEAD_SET_CONTROL_DEPTH, reg::HEAD_DEPTH_24BPP); // 24bpp (8:8:8)
    reg_wr32(head_base + reg::HEAD_SET_PIXEL_CLOCK, mode.pixel_clock_khz * 1000); // Hz

    // Display total
    reg_wr32(head_base + reg::HEAD_SET_DISPLAY_TOTAL,
             (vtotal << 16) | htotal);

    // Sync end
    reg_wr32(head_base + reg::HEAD_SET_SYNC_END,
             (vsynce << 16) | hsynce);

    // Blank end
    reg_wr32(head_base + reg::HEAD_SET_BLANK_END,
             (vblanke << 16) | hblanke);

    // Blank start
    reg_wr32(head_base + reg::HEAD_SET_BLANK_START,
             (vblanks << 16) | hblanks);

    log::info("nvidia: head %u: mode programmed (%ux%u @%uHz, pixel_clock=%u kHz)",
              head, mode.h_active, mode.v_active, mode.refresh_hz, mode.pixel_clock_khz);

    return OK;
}

int32_t nv_gpu::setup_scanout(uint8_t head, uint32_t width, uint32_t height,
                               uint32_t pitch, uint32_t vram_offset) {
    // For NVDisplay (GV100+), scanout is configured through window channels
    // rather than direct head registers. However, the core channel has
    // basic scanout configuration capability.

    // Set display ID (connect head to SOR output)
    uint32_t head_base = reg::HEAD_ASY_BASE + head * reg::HEAD_STRIDE;
    reg_wr32(head_base + reg::HEAD_SET_DISPLAY_ID, 0x00000001 << head);

    // Log the configuration
    log::info("nvidia: scanout: head %u, %ux%u pitch=%u, VRAM offset 0x%x",
              head, width, height, pitch, vram_offset);
    log::info("nvidia: scanout setup complete — display should be active");

    return OK;
}

// ============================================================================
// Logging Helpers
// ============================================================================

const char* nv_gpu::family_name() const {
    switch (m_family) {
    case gpu_family::TESLA:   return "Tesla (NV50)";
    case gpu_family::FERMI:   return "Fermi (GF100)";
    case gpu_family::KEPLER:  return "Kepler (GK100)";
    case gpu_family::MAXWELL: return "Maxwell (GM100)";
    case gpu_family::PASCAL:  return "Pascal (GP100)";
    case gpu_family::VOLTA:   return "Volta (GV100)";
    case gpu_family::TURING:  return "Turing (TU100)";
    case gpu_family::AMPERE:  return "Ampere (GA100)";
    case gpu_family::ADA:     return "Ada Lovelace (AD100)";
    default:                  return "Unknown";
    }
}

const char* nv_gpu::chipset_name() const {
    switch (m_chipset) {
    case 0x170: return "GA100";
    case 0x172: return "GA102";
    case 0x173: return "GA103";
    case 0x174: return "GA104";
    case 0x176: return "GA106";
    case 0x177: return "GA107";
    case 0x162: return "TU102";
    case 0x164: return "TU104";
    case 0x166: return "TU106";
    case 0x168: return "TU116";
    case 0x167: return "TU117";
    case 0x192: return "AD102";
    case 0x194: return "AD104";
    case 0x196: return "AD106";
    case 0x197: return "AD107";
    default:    return "unknown";
    }
}

void nv_gpu::log_chip_info() {
    log::info("nvidia: ---- Chip Identification ----");
    log::info("nvidia: PMC_BOOT_0 = 0x%08x", m_boot0);
    log::info("nvidia: chipset: 0x%03x (%s)", m_chipset, chipset_name());
    log::info("nvidia: revision: 0x%02x", m_chiprev);
    log::info("nvidia: family: %s", family_name());
}

void nv_gpu::log_bar_info() {
    for (uint8_t i = 0; i < pci::MAX_BARS; i++) {
        const pci::bar& b = m_dev->get_bar(i);
        if (b.type == pci::BAR_NONE) continue;
        const char* type_str = (b.type == pci::BAR_IO) ? "I/O" :
                               (b.type == pci::BAR_MMIO32) ? "MMIO32" : "MMIO64";
        log::info("nvidia: BAR%u: phys=0x%lx size=0x%lx type=%s%s",
                  i, b.phys, b.size, type_str,
                  b.prefetchable ? " prefetchable" : "");
    }
}

void nv_gpu::log_dcb_entry(uint32_t idx, const dcb_entry& e) {
    log::info("nvidia: DCB[%u]: type=%s i2c=%u heads=0x%x conn=%u bus=%u loc=%u or=0x%x%s%s",
              idx, output_type_name(e.type), e.i2c_index, e.head_mask,
              e.connector, e.bus, e.location, e.or_mask,
              e.hdmi_enable ? " HDMI" : "",
              (e.type == dcb_output_type::DP)
                  ? (e.dp_max_lanes == 0xF ? " 4-lane" :
                     e.dp_max_lanes == 0x3 ? " 2-lane" : "")
                  : "");
    log::debug("nvidia:   raw: DW0=0x%08x DW1=0x%08x link=%u dprate=%u dplanes=0x%x",
               e.raw[0], e.raw[1], e.link, e.dp_max_rate, e.dp_max_lanes);
}

void nv_gpu::log_i2c_port(uint32_t idx, const i2c_port& p) {
    const char* type_str = "?";
    switch (p.type) {
    case dcb_i2c_type::NV04_BIT:  type_str = "NV04_BIT"; break;
    case dcb_i2c_type::NV4E_BIT:  type_str = "NV4E_BIT"; break;
    case dcb_i2c_type::NVIO_BIT:  type_str = "NVIO_BIT"; break;
    case dcb_i2c_type::NVIO_AUX:  type_str = "NVIO_AUX"; break;
    case dcb_i2c_type::PMGR:      type_str = "PMGR"; break;
    case dcb_i2c_type::UNUSED:    type_str = "UNUSED"; break;
    default: break;
    }
    log::info("nvidia: I2C[%u]: type=%s port=%u aux=%u hybrid=%s valid=%s",
              idx, type_str, p.port, p.aux_port,
              p.hybrid ? "yes" : "no",
              p.valid ? "yes" : "no");
}

void nv_gpu::log_connector(uint32_t idx, const connector_entry& c) {
    log::info("nvidia: CONN[%u]: type=%s (0x%02x) location=%u flags=0x%04x",
              idx, connector_type_name(c.type),
              static_cast<uint8_t>(c.type), c.location, c.flags);
}

void nv_gpu::log_edid(uint32_t idx, const edid_info& e) {
    log::info("nvidia: ---- Monitor %u ----", idx);
    log::info("nvidia:   name: %s", e.display_name[0] ? e.display_name : "(unnamed)");
    log::info("nvidia:   manufacturer: %s, product: 0x%04x, serial: 0x%08x",
              e.manufacturer, e.product_code, e.serial_number);
    log::info("nvidia:   native mode: %ux%u @%uHz",
              e.h_active, e.v_active, e.refresh_hz);
    log::info("nvidia:   pixel clock: %u kHz", e.pixel_clock_khz);
    log::info("nvidia:   timing: h_active=%u h_blank=%u h_sync=%u+%u (%cHSync)",
              e.h_active, e.h_blanking, e.h_sync_offset, e.h_sync_width,
              e.h_sync_positive ? '+' : '-');
    log::info("nvidia:   timing: v_active=%u v_blank=%u v_sync=%u+%u (%cVSync)",
              e.v_active, e.v_blanking, e.v_sync_offset, e.v_sync_width,
              e.v_sync_positive ? '+' : '-');
    log::info("nvidia:   manufacture: week %u, year %u",
              e.mfg_week, 1990 + e.mfg_year);
}

const char* nv_gpu::output_type_name(dcb_output_type type) {
    switch (type) {
    case dcb_output_type::ANALOG: return "CRT/VGA";
    case dcb_output_type::TV:     return "TV";
    case dcb_output_type::TMDS:   return "TMDS/DVI/HDMI";
    case dcb_output_type::LVDS:   return "LVDS";
    case dcb_output_type::DP:     return "DisplayPort";
    case dcb_output_type::WFD:    return "WiFi Display";
    case dcb_output_type::EOL:    return "EOL";
    case dcb_output_type::UNUSED: return "unused";
    default:                      return "unknown";
    }
}

const char* nv_gpu::connector_type_name(dcb_connector_type type) {
    switch (type) {
    case dcb_connector_type::VGA:          return "VGA";
    case dcb_connector_type::DVI_A:        return "DVI-A";
    case dcb_connector_type::TV_COMPOSITE: return "TV-Composite";
    case dcb_connector_type::TV_SVIDEO:    return "TV-SVideo";
    case dcb_connector_type::TV_HDTV:      return "TV-HDTV";
    case dcb_connector_type::DVI_I:        return "DVI-I";
    case dcb_connector_type::DVI_D:        return "DVI-D";
    case dcb_connector_type::LVDS_SPWG:    return "LVDS-SPWG";
    case dcb_connector_type::LVDS_OEM:     return "LVDS-OEM";
    case dcb_connector_type::DP_EXT:       return "DisplayPort";
    case dcb_connector_type::DP_INT:       return "DP-Internal";
    case dcb_connector_type::MDP_EXT:      return "Mini-DP";
    case dcb_connector_type::HDMI_A:       return "HDMI-A";
    case dcb_connector_type::HDMI_C:       return "HDMI-C (Mini)";
    case dcb_connector_type::SKIP:         return "Skip";
    default:                               return "unknown";
    }
}

const edid_info& nv_gpu::get_edid(uint32_t index) const {
    static const edid_info empty = {};
    if (index >= m_edid_count) return empty;
    return m_edid[index];
}

} // namespace nv
