#include "drivers/usb/xhci/tegra_xusb.h"

#if defined(STLX_PLATFORM_JETSON_NANO)

#include "common/logging.h"
#include "common/string.h"
#include "dma/dma.h"
#include "dynpriv/dynpriv.h"
#include "fs/fs.h"
#include "fs/fstypes.h"
#include "hw/delay.h"
#include "hw/mmio.h"
#include "irq/irq.h"
#include "irq/irq_arch.h"
#include "mm/paging_types.h"
#include "mm/vmm.h"

namespace tegra_xusb {

// Register dumps and per-step bring-up logs. Serial logging is synchronous,
// so leaving these on adds over a second to USB bring-up.
constexpr bool VERBOSE = false;

namespace {

constexpr uint64_t FPCI_PHYS = 0x70098000;
constexpr uint64_t IPFS_PHYS = 0x70099000;
constexpr size_t   BLOCK_SIZE = 0x1000;

// IPFS (SoC bus to XUSB bridge)
constexpr uint32_t IPFS_HOST_CONFIGURATION = 0x180;
constexpr uint32_t IPFS_EN_FPCI            = 1u << 0;
constexpr uint32_t IPFS_HOST_INTR_MASK     = 0x188;
constexpr uint32_t IPFS_IP_INT_MASK        = 1u << 16;
constexpr uint32_t IPFS_HOST_CLKGATE_HYST  = 0x1bc;

// FPCI (controller config space, mailbox and CSB window)
constexpr uint32_t CFG_1        = 0x004; // command register
constexpr uint32_t CFG_1_EN     = 0x7;   // IO, memory, bus master
constexpr uint32_t CFG_4        = 0x010; // BAR0
constexpr uint32_t CFG_4_MASK   = 0xFFFF8000;
constexpr uint32_t CSBRANGE     = 0x41c;
constexpr uint32_t CSB_WINDOW   = 0x800;

// Mailbox
constexpr uint32_t MBOX_CMD      = 0x0e4;
constexpr uint32_t MBOX_DATA_IN  = 0x0e8;
constexpr uint32_t MBOX_DATA_OUT = 0x0ec;
constexpr uint32_t MBOX_OWNER    = 0x0f0;
constexpr uint32_t SMI_INTR      = 0x428;

constexpr uint32_t MBOX_DEST_FALC = 1u << 27;
constexpr uint32_t MBOX_DEST_SMI  = 1u << 29;
constexpr uint32_t MBOX_INT_EN    = 1u << 31;
constexpr uint32_t OWNER_NONE     = 0;
constexpr uint32_t OWNER_SW       = 2;
constexpr uint32_t SMI_FW_HANG    = 1u << 1;

constexpr uint32_t MSG_ENABLED        = 1;
constexpr uint32_t MSG_INC_FALC_CLOCK = 2;
constexpr uint32_t MSG_DEC_FALC_CLOCK = 3;
constexpr uint32_t MSG_INC_SSPI_CLOCK = 4;
constexpr uint32_t MSG_DEC_SSPI_CLOCK = 5;
constexpr uint32_t MSG_SET_BW         = 6;
constexpr uint32_t MSG_ACK            = 128;
constexpr uint32_t MSG_NAK            = 129;

// Falcon CSB registers
constexpr uint32_t FALC_CPUCTL        = 0x000100;
constexpr uint32_t FALC_BOOTVEC       = 0x000104;
constexpr uint32_t FALC_DMACTL        = 0x00010c;
constexpr uint32_t FALC_IMFILLRNG1    = 0x000154;
constexpr uint32_t FALC_IMFILLCTL     = 0x000158;
constexpr uint32_t MP_APMAP           = 0x10181c;
constexpr uint32_t MP_ILOAD_ATTR      = 0x101a00;
constexpr uint32_t MP_ILOAD_BASE_LO   = 0x101a04;
constexpr uint32_t MP_ILOAD_BASE_HI   = 0x101a08;
constexpr uint32_t MP_L2IMEMOP_SIZE   = 0x101a10;
constexpr uint32_t MP_L2IMEMOP_TRIG   = 0x101a14;
constexpr uint32_t MP_L2IMEMOP_RESULT = 0x101a18;

constexpr uint32_t APMAP_BOOTPATH          = 0x80000000;
constexpr uint32_t L2IMEMOP_INVALIDATE_ALL = 0x40000000;
constexpr uint32_t L2IMEMOP_LOAD_LOCKED    = 0x11000000;
constexpr uint32_t L2IMEMOP_RESULT_VLD     = 1u << 31;
constexpr uint32_t CPUCTL_STARTCPU         = 1u << 1;

// xHCI operational register used to detect firmware readiness
constexpr uint32_t USBSTS_CNR = 1u << 11;

// Firmware image header (little-endian, 256 bytes)
constexpr size_t   FW_HEADER_SIZE      = 0x100;
constexpr uint32_t FW_BOOT_CODETAG     = 0x08;
constexpr uint32_t FW_BOOT_CODESIZE    = 0x0c;
constexpr uint32_t FW_CREATED_TIME     = 0x2c;
constexpr uint32_t FW_VERSION_ID       = 0x48;
constexpr uint32_t FW_IMG_LEN          = 0x64;
constexpr uint32_t FW_MAGIC            = 0x68;
constexpr const char* FIRMWARE_PATH    = "/lib/firmware/nvidia/tegra210/xusb.bin";

__PRIVILEGED_BSS uintptr_t g_fpci;
__PRIVILEGED_BSS uintptr_t g_ipfs;
__PRIVILEGED_BSS dma::buffer g_fw;

inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

__PRIVILEGED_CODE uintptr_t map_block(uint64_t phys, size_t size) {
    uintptr_t base = 0;
    uintptr_t va = 0;
    if (vmm::map_device(static_cast<pmm::phys_addr_t>(phys), size,
                        paging::PAGE_KERNEL_RW, base, va) != vmm::OK) {
        return 0;
    }
    return va;
}

__PRIVILEGED_CODE uint32_t fpci_read(uint32_t off) { return mmio::read32(g_fpci + off); }
__PRIVILEGED_CODE void fpci_write(uint32_t off, uint32_t v) { mmio::write32(g_fpci + off, v); }
__PRIVILEGED_CODE uint32_t ipfs_read(uint32_t off) { return mmio::read32(g_ipfs + off); }
__PRIVILEGED_CODE void ipfs_write(uint32_t off, uint32_t v) { mmio::write32(g_ipfs + off, v); }

__PRIVILEGED_CODE uint32_t csb_read(uint32_t addr) {
    fpci_write(CSBRANGE, (addr >> 9) & 0x7FFFFF);
    return fpci_read(CSB_WINDOW + (addr & 0x1FF));
}

__PRIVILEGED_CODE void csb_write(uint32_t addr, uint32_t v) {
    fpci_write(CSBRANGE, (addr >> 9) & 0x7FFFFF);
    fpci_write(CSB_WINDOW + (addr & 0x1FF), v);
}

/**
 * Enable the FPCI path, point BAR0 at the xHCI window, enable decoding and
 * bus mastering, and unmask the IP interrupt towards the GIC.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void configure_ipfs_fpci() {
    ipfs_write(IPFS_HOST_CONFIGURATION, ipfs_read(IPFS_HOST_CONFIGURATION) | IPFS_EN_FPCI);
    delay::us(20);

    uint32_t bar0 = fpci_read(CFG_4);
    bar0 = (bar0 & ~CFG_4_MASK) | (static_cast<uint32_t>(XHCI_PHYS) & CFG_4_MASK);
    fpci_write(CFG_4, bar0);
    delay::us(200);

    fpci_write(CFG_1, fpci_read(CFG_1) | CFG_1_EN);

    ipfs_write(IPFS_HOST_INTR_MASK, ipfs_read(IPFS_HOST_INTR_MASK) | IPFS_IP_INT_MASK);
    ipfs_write(IPFS_HOST_CLKGATE_HYST, 0x80);

    if (VERBOSE) {
        log::info("tegra_xusb: FPCI BAR0=0x%08x CMD=0x%08x IPFS cfg=0x%08x mask=0x%08x",
                  fpci_read(CFG_4), fpci_read(CFG_1),
                  ipfs_read(IPFS_HOST_CONFIGURATION), ipfs_read(IPFS_HOST_INTR_MASK));
    }
}

/**
 * Read the firmware image from the initrd into a non-cacheable DMA buffer.
 * The buffer stays allocated for the controller's lifetime.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t read_firmware(uint32_t& fw_len) {
    fs::file* f = fs::open(FIRMWARE_PATH, fs::O_RDONLY);
    if (!f) {
        log::error("tegra_xusb: firmware %s not found", FIRMWARE_PATH);
        return ERR_FIRMWARE;
    }

    fs::vattr attr;
    if (fs::fstat(f, &attr) != fs::OK || attr.size < FW_HEADER_SIZE) {
        fs::close(f);
        log::error("tegra_xusb: firmware too small");
        return ERR_FIRMWARE;
    }

    size_t pages = (attr.size + paging::PAGE_SIZE_4KB - 1) / paging::PAGE_SIZE_4KB;
    if (dma::alloc_pages(pages, g_fw, pmm::ZONE_DMA32) != dma::OK) {
        fs::close(f);
        log::error("tegra_xusb: no DMA memory for firmware (%lu bytes)", attr.size);
        return ERR_NO_MEM;
    }

    ssize_t n = fs::read(f, reinterpret_cast<void*>(g_fw.virt), attr.size);
    fs::close(f);
    if (n < 0 || static_cast<size_t>(n) != attr.size) {
        log::error("tegra_xusb: firmware read failed (%ld)", static_cast<int64_t>(n));
        return ERR_FIRMWARE;
    }

    const auto* img = reinterpret_cast<const uint8_t*>(g_fw.virt);
    if (string::memcmp(img + FW_MAGIC, "XUSBFW", 6) != 0) {
        log::error("tegra_xusb: firmware magic mismatch");
        return ERR_FIRMWARE;
    }

    fw_len = le32(img + FW_IMG_LEN);
    if (fw_len < FW_HEADER_SIZE || fw_len > attr.size) {
        log::error("tegra_xusb: firmware length %u exceeds file size %lu", fw_len, attr.size);
        return ERR_FIRMWARE;
    }

    log::info("tegra_xusb: firmware %u bytes version 0x%08x built %u, at phys 0x%lx",
              fw_len, le32(img + FW_VERSION_ID), le32(img + FW_CREATED_TIME), g_fw.phys);
    return OK;
}

/**
 * Load the firmware into the Falcon and start it, then wait for the xHCI
 * controller to leave the "not ready" state.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t load_firmware(uintptr_t xhci) {
    uint32_t pci_id = fpci_read(0x000);
    if (VERBOSE) {
        log::info("tegra_xusb: FPCI vendor/device id 0x%08x (expect 0x0fac10de)", pci_id);
    }
    if (pci_id != 0x0fac10de) {
        log::error("tegra_xusb: unexpected XUSB controller id, not loading firmware");
        return ERR_FIRMWARE;
    }

    uint32_t fw_len = 0;
    int32_t rc = read_firmware(fw_len);
    if (rc != OK) {
        return rc;
    }

    const auto* img = reinterpret_cast<const uint8_t*>(g_fw.virt);
    uint32_t code_tag  = le32(img + FW_BOOT_CODETAG);
    uint32_t code_size = le32(img + FW_BOOT_CODESIZE);

    // Step-by-step markers: a stalled access here hangs the bus silently, so
    // the last line printed identifies the access that never completed.
    if (VERBOSE) {
        log::info("tegra_xusb: csb probe: reading CSBRANGE (FPCI+0x41c)");
    }
    uint32_t range = fpci_read(CSBRANGE);
    if (VERBOSE) {
        log::info("tegra_xusb: csb probe: CSBRANGE=0x%08x, writing page 0x%x", range,
                  (MP_ILOAD_BASE_LO >> 9) & 0x7FFFFF);
    }
    fpci_write(CSBRANGE, (MP_ILOAD_BASE_LO >> 9) & 0x7FFFFF);
    if (VERBOSE) {
        log::info("tegra_xusb: csb probe: CSBRANGE readback=0x%08x, reading window", fpci_read(CSBRANGE));
    }
    uint32_t iload_lo = fpci_read(CSB_WINDOW + (MP_ILOAD_BASE_LO & 0x1FF));
    if (VERBOSE) {
        log::info("tegra_xusb: csb probe: ILOAD_BASE_LO=0x%08x", iload_lo);
    }

    if (csb_read(MP_ILOAD_BASE_LO) != 0) {
        log::warn("tegra_xusb: firmware base already set (0x%08x), reloading anyway",
                  csb_read(MP_ILOAD_BASE_LO));
    }

    uint64_t code_phys = g_fw.phys + FW_HEADER_SIZE;
    uint32_t tag_blk  = (code_tag + 255) / 256;
    uint32_t size_blk = (code_size + 255) / 256;

    struct step { const char* name; uint32_t reg; uint32_t value; };
    const step steps[] = {
        { "ILOAD_ATTR",        MP_ILOAD_ATTR,    fw_len },
        { "ILOAD_BASE_HI",     MP_ILOAD_BASE_HI, static_cast<uint32_t>(code_phys >> 32) },
        { "ILOAD_BASE_LO",     MP_ILOAD_BASE_LO, static_cast<uint32_t>(code_phys) },
        { "APMAP",             MP_APMAP,         APMAP_BOOTPATH },
        { "L2IMEMOP invalidate", MP_L2IMEMOP_TRIG, L2IMEMOP_INVALIDATE_ALL },
        { "L2IMEMOP_SIZE",     MP_L2IMEMOP_SIZE, ((tag_blk & 0x3ff) << 8) | ((size_blk & 0xff) << 24) },
        { "L2IMEMOP load",     MP_L2IMEMOP_TRIG, L2IMEMOP_LOAD_LOCKED },
        { "FALC_IMFILLCTL",    FALC_IMFILLCTL,   size_blk },
        { "FALC_IMFILLRNG1",   FALC_IMFILLRNG1,  (tag_blk & 0xffff) | (((tag_blk + size_blk) & 0xffff) << 16) },
        { "FALC_DMACTL",       FALC_DMACTL,      0 },
    };
    for (const step& st : steps) {
        if (VERBOSE) {
            log::info("tegra_xusb: fw load: %s (csb 0x%06x) <- 0x%08x", st.name, st.reg, st.value);
        }
        csb_write(st.reg, st.value);
    }
    delay::us(50000);
    if (VERBOSE) {
        log::info("tegra_xusb: fw load: polling L2IMEMOP_RESULT");
    }

    bool loaded = false;
    for (uint32_t waited = 0; waited <= 10000; waited += 100) {
        if (csb_read(MP_L2IMEMOP_RESULT) & L2IMEMOP_RESULT_VLD) {
            loaded = true;
            break;
        }
        delay::us(100);
    }
    if (!loaded) {
        log::error("tegra_xusb: boot code DMA did not complete (result=0x%08x)",
                   csb_read(MP_L2IMEMOP_RESULT));
        return ERR_TIMEOUT;
    }
    if (VERBOSE) {
        log::info("tegra_xusb: fw load: boot code in IMEM, starting Falcon at 0x%x", code_tag);
    }

    csb_write(FALC_BOOTVEC, code_tag);
    csb_write(FALC_CPUCTL, CPUCTL_STARTCPU);
    delay::us(2000);
    if (VERBOSE) {
        log::info("tegra_xusb: fw load: Falcon started, CPUCTL=0x%08x", csb_read(FALC_CPUCTL));
    }

    uint32_t caplength = mmio::read32(xhci) & 0xFF;
    uintptr_t usbsts = xhci + caplength + 0x04;
    for (uint32_t waited = 0; waited <= 200; waited++) {
        if ((mmio::read32(usbsts) & USBSTS_CNR) == 0) {
            log::info("tegra_xusb: firmware running after %u ms (USBSTS=0x%08x, HCIVERSION=0x%04x)",
                      waited, mmio::read32(usbsts), mmio::read32(xhci) >> 16);
            return OK;
        }
        delay::us(1000);
    }

    log::error("tegra_xusb: controller still not ready, CPUCTL=0x%08x USBSTS=0x%08x",
               csb_read(FALC_CPUCTL), mmio::read32(usbsts));
    return ERR_TIMEOUT;
}

__PRIVILEGED_CODE void mbox_reply(uint32_t cmd, uint32_t data) {
    fpci_write(MBOX_DATA_IN, (cmd << 24) | (data & 0xFFFFFF));
    fpci_write(MBOX_CMD, fpci_read(MBOX_CMD) | MBOX_INT_EN | MBOX_DEST_FALC);
}

/**
 * Firmware-to-host mailbox. Clock requests are acknowledged with the
 * requested value (the host does not rescale Falcon/SS clocks on Tegra210),
 * SET_BW needs no reply, and anything unrecognised is acknowledged so the
 * mailbox is never left owned by the firmware.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mbox_isr(uint32_t, void*) {
    uint32_t smi = fpci_read(SMI_INTR);
    fpci_write(SMI_INTR, smi);
    if (smi & SMI_FW_HANG) {
        log::error("tegra_xusb: firmware reported a hang");
    }

    uint32_t msg = fpci_read(MBOX_DATA_OUT);
    fpci_write(MBOX_CMD, fpci_read(MBOX_CMD) & ~MBOX_DEST_SMI);

    uint32_t cmd = msg >> 24;
    uint32_t data = msg & 0xFFFFFF;

    if (cmd == MSG_SET_BW || cmd == MSG_ACK || cmd == MSG_NAK) {
        fpci_write(MBOX_OWNER, OWNER_NONE);
        return;
    }

    switch (cmd) {
    case MSG_INC_FALC_CLOCK:
    case MSG_DEC_FALC_CLOCK:
    case MSG_INC_SSPI_CLOCK:
    case MSG_DEC_SSPI_CLOCK:
        break;
    default:
        if (VERBOSE) {
            log::info("tegra_xusb: mailbox message %u data 0x%06x acknowledged", cmd, data);
        }
        break;
    }
    mbox_reply(MSG_ACK, data);
}

// ---------------------------------------------------------------------------
// Power partitions, clocks, resets and USB2 pads
// ---------------------------------------------------------------------------

constexpr uint64_t CAR_PHYS    = 0x60006000;
constexpr uint64_t GPIO_PHYS   = 0x6000d000;
constexpr uint64_t MISC_PHYS   = 0x70000000; // APB_MISC incl. pinmux at +0x3000
constexpr size_t   MISC_SIZE   = 0x4000;
constexpr uint64_t FUSE_PHYS   = 0x7000f000;
constexpr uint64_t PADCTL_PHYS = 0x7009f000;

// CAR per-bank set/clear registers
struct car_bank { uint32_t enb_set, enb_clr, rst_set, rst_clr; };
constexpr car_bank CAR_U = { 0x330, 0x334, 0x310, 0x314 };
constexpr car_bank CAR_W = { 0x448, 0x44c, 0x438, 0x43c };
constexpr car_bank CAR_Y = { 0x29c, 0x2a0, 0x2a8, 0x2ac };

constexpr uint32_t BIT_XUSB_HOST = 1u << 25; // U25, id 89
constexpr uint32_t BIT_XUSB_DEV  = 1u << 31; // U31, id 95
constexpr uint32_t BIT_XUSB_SS   = 1u << 28; // W28, id 156
constexpr uint32_t BIT_XUSB_GATE = 1u << 15; // W15, id 143
constexpr uint32_t BIT_USB2_TRK  = 1u << 18; // Y18, id 210

constexpr uint32_t CLK_OUT_ENB_U = 0x018;
constexpr uint32_t CLK_OUT_ENB_W = 0x364;
constexpr uint32_t RST_DEV_U     = 0x00c;
constexpr uint32_t RST_DEV_W     = 0x35c;

constexpr uint32_t CLK_SRC_XUSB_HOST   = 0x600;
constexpr uint32_t CLK_SRC_XUSB_FALCON = 0x604;
constexpr uint32_t CLK_SRC_XUSB_FS     = 0x608;
constexpr uint32_t CLK_SRC_XUSB_DEV    = 0x60c;
constexpr uint32_t CLK_SRC_XUSB_SS     = 0x610;
constexpr uint32_t LVL2_CLK_GATE_OVRC  = 0x3a0;

constexpr uint32_t PLLP_MISC1          = 0x680;
constexpr uint32_t PLLU_BASE           = 0x0c0;
constexpr uint32_t PLLU_MISC0          = 0x0cc;
constexpr uint32_t UTMIP_PLL_CFG1      = 0x484;
constexpr uint32_t UTMIP_PLL_CFG2      = 0x488;
constexpr uint32_t UTMIPLL_HW_PWRDN    = 0x52c;
constexpr uint32_t PLLU_HW_PWRDN       = 0x530;
constexpr uint32_t XUSB_PLL_CFG0       = 0x534;

// PMC via TF-A (the PMC is secure-only on this board)
constexpr uint64_t SIP_PMC_FID      = 0xC2FFFE00;
constexpr uint64_t SIP_PMC_FID_NEW  = 0xC200FE00;
constexpr uint64_t SIP_PMC_READ     = 0xAA;
constexpr uint64_t SIP_PMC_WRITE    = 0xBB;
constexpr uint32_t PMC_PWRGATE_TOGGLE = 0x30;
constexpr uint32_t PMC_REMOVE_CLAMP   = 0x34;
constexpr uint32_t PMC_PWRGATE_STATUS = 0x38;
constexpr uint32_t PMC_IO_DPD_STATUS  = 0x1bc;
constexpr uint32_t PWRGATE_START      = 1u << 8;
constexpr uint32_t PARTITION_XUSBA    = 20;
constexpr uint32_t PARTITION_XUSBC    = 22;

// Pad controller
constexpr uint32_t PADCTL_USB2_PAD_MUX      = 0x004;
constexpr uint32_t PADCTL_USB2_PORT_CAP     = 0x008;
constexpr uint32_t PADCTL_ELPG_PROGRAM1     = 0x024;
constexpr uint32_t PADCTL_BIAS_PAD_CTL0     = 0x284;
constexpr uint32_t PADCTL_BIAS_PAD_CTL1     = 0x288;
constexpr uint32_t HUB_PORT = 1; // every USB-A port sits behind the hub on usb2-1

constexpr uint32_t padctl_batt_ctl1(uint32_t p) { return 0x084 + p * 0x40; }
constexpr uint32_t padctl_otg_ctl0(uint32_t p)  { return 0x088 + p * 0x40; }
constexpr uint32_t padctl_otg_ctl1(uint32_t p)  { return 0x08c + p * 0x40; }

__PRIVILEGED_BSS uintptr_t g_car;
__PRIVILEGED_BSS uintptr_t g_padctl;
__PRIVILEGED_BSS uint64_t g_pmc_fid;

__PRIVILEGED_CODE uint32_t car_read(uint32_t off) { return mmio::read32(g_car + off); }
__PRIVILEGED_CODE void car_write(uint32_t off, uint32_t v) { mmio::write32(g_car + off, v); }
__PRIVILEGED_CODE uint32_t pad_read(uint32_t off) { return mmio::read32(g_padctl + off); }
__PRIVILEGED_CODE void pad_write(uint32_t off, uint32_t v) { mmio::write32(g_padctl + off, v); }

__PRIVILEGED_CODE void car_update(uint32_t off, uint32_t clear, uint32_t set) {
    car_write(off, (car_read(off) & ~clear) | set);
}

__PRIVILEGED_CODE void pad_update(uint32_t off, uint32_t clear, uint32_t set) {
    pad_write(off, (pad_read(off) & ~clear) | set);
}

struct smc_result { uint64_t x0; uint64_t x1; };

__PRIVILEGED_CODE smc_result smc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t x0 asm("x0") = fid;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    asm volatile("smc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        :
        : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
          "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    return { x0, x1 };
}

__PRIVILEGED_CODE bool pmc_read(uint32_t off, uint32_t& out) {
    smc_result r = smc(g_pmc_fid, SIP_PMC_READ, off, 0);
    if (r.x0 != 0) return false;
    out = static_cast<uint32_t>(r.x1);
    return true;
}

__PRIVILEGED_CODE bool pmc_write(uint32_t off, uint32_t v) {
    return smc(g_pmc_fid, SIP_PMC_WRITE, off, v).x0 == 0;
}

/**
 * Find which SiP function ID this TF-A build answers for PMC access.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool pmc_probe() {
    const uint64_t fids[] = { SIP_PMC_FID, SIP_PMC_FID_NEW };
    for (uint64_t fid : fids) {
        smc_result r = smc(fid, SIP_PMC_READ, PMC_PWRGATE_STATUS, 0);
        if (VERBOSE) {
            log::info("tegra_xusb: PMC SMC 0x%lx read status -> x0=0x%lx x1=0x%lx", fid, r.x0, r.x1);
        }
        if (r.x0 == 0) {
            g_pmc_fid = fid;
            return true;
        }
    }
    return false;
}

template <typename Pred>
__PRIVILEGED_CODE bool poll_us(uint32_t timeout_us, uint32_t step_us, Pred done) {
    for (uint32_t waited = 0; waited <= timeout_us; waited += step_us) {
        if (done()) return true;
        delay::us(step_us);
    }
    return false;
}

/**
 * Snapshot of everything the bootloader may or may not have configured.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dump_state(const char* when) {
    uint32_t pg = 0, dpd = 0;
    bool pg_ok = pmc_read(PMC_PWRGATE_STATUS, pg);
    bool dpd_ok = pmc_read(PMC_IO_DPD_STATUS, dpd);
    if (VERBOSE) {
        log::info("tegra_xusb: [%s] CLK U=%08x W=%08x RST U=%08x W=%08x PWRGATE=%s%08x IO_DPD=%s%08x",
                  when, car_read(CLK_OUT_ENB_U), car_read(CLK_OUT_ENB_W),
                  car_read(RST_DEV_U), car_read(RST_DEV_W),
                  pg_ok ? "" : "?", pg, dpd_ok ? "" : "?", dpd);
    }
    if (VERBOSE) {
        log::info("tegra_xusb: [%s] PLLU_BASE=%08x PLLU_MISC0=%08x UTMIPLL_PWRDN=%08x PLLU_PWRDN=%08x PLLP_MISC1=%08x",
                  when, car_read(PLLU_BASE), car_read(PLLU_MISC0), car_read(UTMIPLL_HW_PWRDN),
                  car_read(PLLU_HW_PWRDN), car_read(PLLP_MISC1));
    }
    if (VERBOSE) {
        log::info("tegra_xusb: [%s] SRC host=%08x falcon=%08x fs=%08x ss=%08x",
                  when, car_read(CLK_SRC_XUSB_HOST), car_read(CLK_SRC_XUSB_FALCON),
                  car_read(CLK_SRC_XUSB_FS), car_read(CLK_SRC_XUSB_SS));
    }
    if (VERBOSE) {
        log::info("tegra_xusb: [%s] PADCTL mux=%08x cap=%08x elpg1=%08x bias0=%08x bias1=%08x",
                  when, pad_read(PADCTL_USB2_PAD_MUX), pad_read(PADCTL_USB2_PORT_CAP),
                  pad_read(PADCTL_ELPG_PROGRAM1), pad_read(PADCTL_BIAS_PAD_CTL0),
                  pad_read(PADCTL_BIAS_PAD_CTL1));
    }
    if (VERBOSE) {
        log::info("tegra_xusb: [%s] PAD%u batt1=%08x otg0=%08x otg1=%08x",
                  when, HUB_PORT, pad_read(padctl_batt_ctl1(HUB_PORT)),
                  pad_read(padctl_otg_ctl0(HUB_PORT)), pad_read(padctl_otg_ctl1(HUB_PORT)));
    }
}

/**
 * Drive GPIO A6 high: VDD_HUB_3V3, the onboard USB hub's supply.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void hub_power_on(uintptr_t misc, uintptr_t gpio) {
    constexpr uint32_t PINMUX_PA6 = 0x3244;
    constexpr uint32_t PINMUX_TRISTATE = 1u << 4;
    uint32_t pm = mmio::read32(misc + PINMUX_PA6);
    mmio::write32(misc + PINMUX_PA6, pm & ~PINMUX_TRISTATE);

    // Masked writes: bit 14 enables the write of bit 6 (port A pin 6)
    mmio::write32(gpio + 0xa0, 0x4040); // OUT high
    mmio::write32(gpio + 0x90, 0x4040); // output enable
    mmio::write32(gpio + 0x80, 0x4040); // pin is GPIO

    if (VERBOSE) {
        log::info("tegra_xusb: hub power GPIO A6 pinmux %08x->%08x cnf=%08x oe=%08x out=%08x in=%08x",
                  pm, mmio::read32(misc + PINMUX_PA6), mmio::read32(gpio + 0x00),
                  mmio::read32(gpio + 0x10), mmio::read32(gpio + 0x20), mmio::read32(gpio + 0x30));
    }
}

/**
 * PLLP XUSB output, PLLU (480 MHz, handed to hardware control) and UTMIPLL.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t enable_plls() {
    car_update(PLLP_MISC1, 0, (1u << 28) | (1u << 29));

    uint32_t pllu = car_read(PLLU_BASE);
    if (pllu & (1u << 24)) { // still under software override
        if ((pllu & (1u << 30)) == 0) {
            car_update(PLLU_MISC0, 1u << 31, 0); // leave IDDQ
            delay::us(5);
            car_update(PLLU_BASE, 0x1FFFFF, 0x00011902); // M=2 N=25 P=1 from 38.4 MHz
            car_update(PLLU_BASE, 0, 1u << 30);
            if (!poll_us(1000, 10, [] { return (car_read(PLLU_BASE) & (1u << 27)) != 0; })) {
                log::error("tegra_xusb: PLLU failed to lock (base=%08x)", car_read(PLLU_BASE));
                return ERR_POWER;
            }
        }
        car_update(PLLU_BASE, 1u << 24, 0);
        car_update(PLLU_HW_PWRDN, (1u << 2) | (1u << 0), (1u << 28) | (1u << 7) | (1u << 6));
        car_update(XUSB_PLL_CFG0, 0x3FFu << 14, 0);
        car_update(PLLU_HW_PWRDN, 0, 1u << 24);
        car_update(PLLU_BASE, 1u << 21, 0);
    }
    car_update(PLLU_BASE, 0, (1u << 22) | (1u << 25)); // 480M and 48M outputs

    if ((car_read(UTMIPLL_HW_PWRDN) & (1u << 24)) == 0) {
        car_update(UTMIPLL_HW_PWRDN, 1u << 1, 0);
        delay::us(10);
        car_update(UTMIP_PLL_CFG2, (0xFFFu << 6) | (0x3Fu << 18), 6u << 18);
        car_update(UTMIP_PLL_CFG1, (0x1Fu << 27) | 0xFFFu, 0x80 | (1u << 16));
        car_update(UTMIP_PLL_CFG1, 1u << 14, 1u << 15);
        delay::us(20);
        car_update(UTMIP_PLL_CFG2, (1u << 0) | (1u << 2) | (1u << 24),
                   (1u << 1) | (1u << 3) | (1u << 25));
        car_update(UTMIP_PLL_CFG1, (1u << 14) | (1u << 15), 0);
        car_update(UTMIPLL_HW_PWRDN, 1u << 2, 1u << 6);
        delay::us(1);
        car_update(XUSB_PLL_CFG0, 0x3FF, 0);
        delay::us(1);
        car_update(UTMIPLL_HW_PWRDN, 0, 1u << 24);
        if (!poll_us(1000, 10, [] { return (car_read(UTMIPLL_HW_PWRDN) & (1u << 31)) != 0; })) {
            log::warn("tegra_xusb: UTMIPLL lock bit not set (pwrdn=%08x)", car_read(UTMIPLL_HW_PWRDN));
        }
    }
    return OK;
}

/**
 * Power up one partition the way Linux's tegra_powergate_power_up does.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t power_partition(uint32_t id, const car_bank& bank, uint32_t bit) {
    car_write(bank.rst_set, bit);
    delay::us(20);

    uint32_t status = 0;
    if (!pmc_read(PMC_PWRGATE_STATUS, status)) return ERR_POWER;
    if ((status & (1u << id)) == 0) {
        uint32_t toggle = 0;
        auto toggle_idle = [&] { return pmc_read(PMC_PWRGATE_TOGGLE, toggle) && (toggle & PWRGATE_START) == 0; };
        if (!poll_us(100000, 10, toggle_idle)) return ERR_POWER;
        if (!pmc_write(PMC_PWRGATE_TOGGLE, PWRGATE_START | id)) return ERR_POWER;
        if (!poll_us(100000, 10, toggle_idle)) return ERR_POWER;
        auto powered = [&] { return pmc_read(PMC_PWRGATE_STATUS, status) && (status & (1u << id)) != 0; };
        if (!poll_us(100000, 10, powered)) {
            log::error("tegra_xusb: partition %u did not power up (status=%08x)", id, status);
            return ERR_POWER;
        }
    }

    car_write(bank.enb_set, bit);
    delay::us(20);
    if (!pmc_write(PMC_REMOVE_CLAMP, 1u << id)) return ERR_POWER;
    delay::us(20);
    car_write(bank.rst_clr, bit);
    delay::us(20);
    if (VERBOSE) {
        log::info("tegra_xusb: partition %u powered", id);
    }
    return OK;
}

/**
 * Tegra210 MBIST workaround after the XUSB partitions come up.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mbist_workaround() {
    uint32_t dev_on = car_read(CLK_OUT_ENB_U) & BIT_XUSB_DEV;
    car_write(CLK_SRC_XUSB_DEV, 0x20000006);
    car_write(CAR_U.enb_set, BIT_XUSB_HOST | BIT_XUSB_DEV);
    car_write(CAR_W.enb_set, BIT_XUSB_SS);

    uint32_t ovrc = car_read(LVL2_CLK_GATE_OVRC);
    car_write(LVL2_CLK_GATE_OVRC, ovrc | 0xC0000000);
    delay::us(1);
    car_write(LVL2_CLK_GATE_OVRC, ovrc);
    delay::us(1);

    if (!dev_on) car_write(CAR_U.enb_clr, BIT_XUSB_DEV);
}

/**
 * Pad controller setup mirroring NVIDIA's L4T U-Boot for the Nano
 * (tegra_xhci_core_init -> do_tegra_xusb_padctl_init): reset PADCTL, route
 * the USB2 pads to XUSB, program all three OTG pads from the fuses, start
 * bias tracking, then map SS port 0 to USB2 port 1 (the onboard hub) and
 * lift the SuperSpeed and AUX clamps.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void setup_usb2_pad(uintptr_t fuse, uintptr_t misc) {
    constexpr uint32_t BIT_PADCTL = 1u << 14; // W14, id 142
    constexpr uint32_t PADCTL_ELPG_PROGRAM0 = 0x020;
    constexpr uint32_t PADCTL_SS_PORT_MAP   = 0x014;
    constexpr uint32_t PADCTL_USB3_PAD_MUX  = 0x028;

    car_write(CAR_W.rst_set, BIT_PADCTL);
    delay::us(1000);
    car_write(CAR_W.rst_clr, BIT_PADCTL);
    delay::us(10);

    uint32_t calib     = mmio::read32(fuse + 0x9f0);  // FUSE_USB_CALIB
    uint32_t calib_ext = mmio::read32(fuse + 0xa50);  // FUSE_USB_CALIB_EXT
    uint32_t hs_curr[3] = { calib & 0x3F, (calib >> 11) & 0x3F, (calib >> 17) & 0x3F };
    uint32_t squelch   = (calib >> 29) & 0x7;
    uint32_t term_adj  = (calib >> 7) & 0xF;
    uint32_t rpd_ctrl  = calib_ext & 0x1F;
    if (VERBOSE) {
        log::info("tegra_xusb: fuses calib=%08x ext=%08x hs_curr=%u/%u/%u squelch=%u term_adj=%u rpd=%u rev=%u",
                  calib, calib_ext, hs_curr[0], hs_curr[1], hs_curr[2], squelch, term_adj, rpd_ctrl,
                  (mmio::read32(misc + 0x804) >> 16) & 0xF);
    }

    // Bias pad and OTG pads 0-2 owned by XUSB
    pad_update(PADCTL_USB2_PAD_MUX, (3u << 18) | 0x3Fu, (1u << 18) | 0x15u);
    pad_write(PADCTL_ELPG_PROGRAM0, 0x41E00780);      // clear SS/USB2/HSIC wake events
    // Port 0 OTG (micro-B), ports 1-2 host, port 3 disabled
    pad_update(PADCTL_USB2_PORT_CAP, 0xFFFFu, 0x0113u);

    for (uint32_t pad = 0; pad < 3; pad++) {
        pad_update(padctl_otg_ctl0(pad), (1u << 26) | (1u << 29) | 0x3Fu, hs_curr[pad]);
        pad_update(padctl_otg_ctl1(pad), (0xFu << 3) | (0x1Fu << 26) | (1u << 2),
                   (term_adj << 3) | (rpd_ctrl << 26));
    }

    // Bias pad tracking (U-Boot leaves the tracking clock running)
    car_write(CAR_Y.enb_set, BIT_USB2_TRK);
    pad_update(PADCTL_BIAS_PAD_CTL1, (0x7Fu << 12) | (0x7Fu << 19), (0x1Eu << 12) | (0x0Au << 19));
    pad_update(PADCTL_BIAS_PAD_CTL0, (1u << 11) | (7u << 3) | 7u, (7u << 3) | squelch);
    delay::us(1);
    pad_update(PADCTL_BIAS_PAD_CTL1, 1u << 26, 0);

    // SS port 0 belongs to USB2 port 1; unclamp SSP0, SSP3 and the AUX mux
    pad_update(PADCTL_SS_PORT_MAP, 0x7u, 0x1u);
    pad_update(PADCTL_ELPG_PROGRAM1,
               0x7u | (0x7u << 9) | (1u << 29) | (1u << 30) | (1u << 31), 0);
    pad_update(PADCTL_USB3_PAD_MUX, 0, (0x7Fu << 1) | (1u << 8));

    if (VERBOSE) {
        log::info("tegra_xusb: padctl mux=%08x cap=%08x elpg0=%08x elpg1=%08x ssmap=%08x usb3mux=%08x",
                  pad_read(PADCTL_USB2_PAD_MUX), pad_read(PADCTL_USB2_PORT_CAP),
                  pad_read(PADCTL_ELPG_PROGRAM0), pad_read(PADCTL_ELPG_PROGRAM1),
                  pad_read(PADCTL_SS_PORT_MAP), pad_read(PADCTL_USB3_PAD_MUX));
    }
}

/**
 * PLLREFE then PLLE at 100 MHz, as NVIDIA's L4T U-Boot tegra_plle_enable does.
 * Skipped when PLLE is already enabled and locked.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t enable_plle() {
    constexpr uint32_t PLLE_SS_CNTL  = 0x068;
    constexpr uint32_t PLLE_BASE     = 0x0e8;
    constexpr uint32_t PLLE_MISC     = 0x0ec;
    constexpr uint32_t PLLE_AUX      = 0x48c;
    constexpr uint32_t PLLREFE_BASE  = 0x4c4;
    constexpr uint32_t PLLREFE_MISC  = 0x4c8;

    if ((car_read(PLLE_BASE) & (1u << 31)) && (car_read(PLLE_MISC) & (1u << 11))) {
        if (VERBOSE) {
            log::info("tegra_xusb: PLLE already locked (base=%08x misc=%08x)",
                      car_read(PLLE_BASE), car_read(PLLE_MISC));
        }
        return OK;
    }

    car_update(PLLREFE_MISC, 1u << 24, 0);                  // leave IDDQ
    delay::us(5);
    car_write(PLLREFE_BASE, (1u << 30) | (0x41u << 8) | 4); // enable, N=0x41, M=4
    if (!poll_us(250000, 10, [] { return (car_read(PLLREFE_MISC) & (1u << 27)) != 0; })) {
        log::error("tegra_xusb: PLLREFE failed to lock (misc=%08x)", car_read(PLLREFE_MISC));
        return ERR_POWER;
    }

    car_update(PLLE_AUX, 1u << 28, 0);                      // XTAL as PLLE source
    car_update(PLLE_MISC, 1u << 13, 0);
    delay::us(5);
    car_update(PLLE_BASE, (0x1Fu << 24) | (0xFFu << 8) | 0xFFu,
               (0xEu << 24) | (0x7Du << 8) | 2u);           // PLDIV_CML=14 N=125 M=2
    car_update(PLLE_MISC, (3u << 6) | (3u << 2) | 1u, 1u << 8);
    car_update(PLLE_BASE, 0, 1u << 31);
    if (!poll_us(250000, 10, [] { return (car_read(PLLE_MISC) & (1u << 11)) != 0; })) {
        log::error("tegra_xusb: PLLE failed to lock (base=%08x misc=%08x)",
                   car_read(PLLE_BASE), car_read(PLLE_MISC));
        return ERR_POWER;
    }

    uint32_t ss = car_read(PLLE_SS_CNTL);
    ss &= ~((0xFFu << 16) | (0x3Fu << 24) | 0x1FFFu);
    ss |= (1u << 16) | (0x23u << 24) | 0x21u;
    ss &= ~((1u << 15) | (1u << 14) | (1u << 10) | (1u << 12));
    car_write(PLLE_SS_CNTL, ss);
    delay::us(1);
    car_write(PLLE_SS_CNTL, ss & ~(1u << 11));

    if (VERBOSE) {
        log::info("tegra_xusb: PLLE locked (base=%08x misc=%08x)",
                  car_read(PLLE_BASE), car_read(PLLE_MISC));
    }
    return OK;
}

/**
 * PEX/USB UPHY PLL in the pad controller, as NVIDIA's L4T U-Boot
 * tegra_uphy_pll_enable does. Skipped when already enabled or locked.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t enable_uphy_pll() {
    constexpr uint32_t CTL1 = 0x360;
    constexpr uint32_t CTL2 = 0x364;
    constexpr uint32_t CTL4 = 0x36c;
    constexpr uint32_t CTL5 = 0x370;
    constexpr uint32_t CTL8 = 0x37c;
    constexpr uint32_t BIT_PEX_USB_UPHY = 1u << 13; // Y13, id 205

    if (pad_read(CTL1) & ((1u << 15) | (1u << 3))) {
        if (VERBOSE) {
            log::info("tegra_xusb: UPHY PLL already enabled (ctl1=%08x)", pad_read(CTL1));
        }
        return OK;
    }

    car_write(CAR_Y.rst_clr, BIT_PEX_USB_UPHY);
    pad_write(CTL2, 0x136u << 4);
    pad_update(CTL5, 0xFFFF0000u, 0x2Au << 16);
    pad_update(CTL1, 0, 1u << 4);
    pad_update(CTL2, 0, 1u << 2);
    pad_update(CTL8, 0, 1u << 15);
    pad_update(CTL4, (3u << 12) | (0xFu << 4), (2u << 12) | (1u << 15));
    pad_update(CTL1, (0xFFu << 20) | (3u << 16), 0x19u << 20);
    pad_update(CTL1, 7u, 0);
    delay::us(20);
    pad_update(CTL4, 0, 1u << 8);
    if (VERBOSE) {
        log::info("tegra_xusb: UPHY pre-cal ctl1=%08x ctl2=%08x ctl4=%08x ctl5=%08x ctl8=%08x",
                  pad_read(CTL1), pad_read(CTL2), pad_read(CTL4), pad_read(CTL5), pad_read(CTL8));
    }

    pad_update(CTL2, 0, 1u << 0);
    if (!poll_us(20000, 10, [] { return (pad_read(CTL2) & (1u << 1)) != 0; })) {
        log::error("tegra_xusb: UPHY PLL calibration did not finish (ctl2=%08x)", pad_read(CTL2));
        return ERR_POWER;
    }
    pad_update(CTL2, 1u << 0, 0);
    if (!poll_us(20000, 10, [] { return (pad_read(CTL2) & (1u << 1)) == 0; })) {
        log::error("tegra_xusb: UPHY PLL calibration did not clear (ctl2=%08x)", pad_read(CTL2));
        return ERR_POWER;
    }

    pad_update(CTL1, 0, 1u << 3);
    if (!poll_us(20000, 10, [] { return (pad_read(CTL1) & (1u << 15)) != 0; })) {
        log::error("tegra_xusb: UPHY PLL failed to lock (ctl1=%08x)", pad_read(CTL1));
        return ERR_POWER;
    }

    pad_update(CTL8, 0, 3u << 12);
    if (!poll_us(20000, 10, [] { return (pad_read(CTL8) & (1u << 31)) != 0; })) {
        log::error("tegra_xusb: UPHY RCAL did not finish (ctl8=%08x)", pad_read(CTL8));
        return ERR_POWER;
    }
    pad_update(CTL8, 1u << 12, 0);
    if (!poll_us(20000, 10, [] { return (pad_read(CTL8) & (1u << 31)) == 0; })) {
        log::error("tegra_xusb: UPHY RCAL did not clear (ctl8=%08x)", pad_read(CTL8));
        return ERR_POWER;
    }
    pad_update(CTL8, 1u << 13, 0);

    pad_update(CTL1, 1u << 4, 0);
    pad_update(CTL2, 1u << 2, 0);
    pad_update(CTL8, 1u << 15, 0);

    if (VERBOSE) {
        log::info("tegra_xusb: UPHY PLL locked (ctl1=%08x)", pad_read(CTL1));
    }
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t power_clocks_pads() {
    g_car = map_block(CAR_PHYS, BLOCK_SIZE);
    g_padctl = map_block(PADCTL_PHYS, BLOCK_SIZE);
    uintptr_t gpio = map_block(GPIO_PHYS, BLOCK_SIZE);
    uintptr_t misc = map_block(MISC_PHYS, MISC_SIZE);
    uintptr_t fuse = map_block(FUSE_PHYS, BLOCK_SIZE);
    if (!g_car || !g_padctl || !gpio || !misc || !fuse) {
        log::error("tegra_xusb: failed to map CAR/PADCTL/GPIO/MISC/FUSE");
        return ERR_MAP;
    }

    if (!pmc_probe()) {
        log::error("tegra_xusb: secure PMC does not answer SiP SMC calls");
        return ERR_POWER;
    }

    if (VERBOSE) dump_state("entry");

    hub_power_on(misc, gpio);

    int32_t rc = enable_plls();
    if (rc != OK) return rc;

    car_write(CLK_SRC_XUSB_HOST,   0x20000006); // pll_p_out_xusb / 4 = 102 MHz
    car_write(CLK_SRC_XUSB_FALCON, 0x20000002); // pll_p_out_xusb / 2 = 204 MHz
    car_write(CLK_SRC_XUSB_FS,     0x40000000); // pll_u_48M
    car_write(CLK_SRC_XUSB_SS,     0x64000006); // pll_u_480M / 4 = 120 MHz, hs_src = ss_src
    car_write(CAR_W.enb_set, BIT_XUSB_GATE);
    delay::us(10);

    // NVIDIA's U-Boot runs the host with the XUSB_DEV partition clock on and
    // its reset released, and releases the PLLU OUT1 divider and the legacy
    // USBD/USB2 resets before touching the Falcon.
    constexpr uint32_t PLLU_OUTA = 0x0c4;
    car_write(CLK_SRC_XUSB_DEV, 0x20000006);
    car_write(CAR_U.enb_set, BIT_XUSB_DEV);
    delay::us(2);
    car_update(PLLU_OUTA, 0, 1u << 0);
    delay::us(2);
    car_write(CAR_U.rst_clr, BIT_XUSB_DEV);
    delay::us(2);
    car_write(0x304, 1u << 22);   // RST_DEV_L_CLR: USBD
    delay::us(2);
    car_write(0x30c, 1u << 26);   // RST_DEV_H_CLR: USB2
    delay::us(2);

    rc = power_partition(PARTITION_XUSBA, CAR_W, BIT_XUSB_SS);
    if (rc != OK) return rc;
    rc = power_partition(PARTITION_XUSBC, CAR_U, BIT_XUSB_HOST);
    if (rc != OK) return rc;
    mbist_workaround();

    setup_usb2_pad(fuse, misc);

    rc = enable_plle();
    if (rc != OK) return rc;
    rc = enable_uphy_pll();
    if (rc != OK) {
        log::warn("tegra_xusb: UPHY PLL unavailable, continuing with USB2 only");
    }

    if (VERBOSE) dump_state("ready");
    return OK;
}

} // namespace

__PRIVILEGED_CODE int32_t bring_up() {
    int32_t rc = power_clocks_pads();
    if (rc != OK) {
        return rc;
    }

    g_fpci = map_block(FPCI_PHYS, BLOCK_SIZE);
    g_ipfs = map_block(IPFS_PHYS, BLOCK_SIZE);
    uintptr_t xhci = map_block(XHCI_PHYS, BLOCK_SIZE);
    if (!g_fpci || !g_ipfs || !xhci) {
        log::error("tegra_xusb: failed to map XUSB registers");
        return ERR_MAP;
    }

    configure_ipfs_fpci();

    rc = load_firmware(xhci);
    if (rc != OK) {
        return rc;
    }

    if (irq::register_handler(MBOX_IRQ, mbox_isr, nullptr) == irq::OK) {
        irq::set_level_triggered(MBOX_IRQ);
        irq::set_spi_target(MBOX_IRQ, 0x01);
        irq::set_group1(MBOX_IRQ);
        irq::unmask(MBOX_IRQ);
    } else {
        log::warn("tegra_xusb: mailbox IRQ %u registration failed", MBOX_IRQ);
    }

    return OK;
}

void on_controller_started(void*) {
    RUN_ELEVATED({
        if (fpci_read(MBOX_OWNER) != OWNER_NONE) {
            log::warn("tegra_xusb: mailbox busy (owner=%u), MSG_ENABLED not sent",
                      fpci_read(MBOX_OWNER));
        } else {
            fpci_write(MBOX_OWNER, OWNER_SW);
            if (fpci_read(MBOX_OWNER) != OWNER_SW) {
                log::warn("tegra_xusb: could not acquire mailbox");
            } else {
                mbox_reply(MSG_ENABLED, 0);
                bool released = false;
                for (uint32_t waited = 0; waited < 250000; waited += 20) {
                    if (fpci_read(MBOX_OWNER) == OWNER_NONE) {
                        released = true;
                        break;
                    }
                    delay::us(20);
                }
                if (released) {
                    if (VERBOSE) {
                        log::info("tegra_xusb: firmware acknowledged MSG_ENABLED");
                    }
                } else {
                    log::warn("tegra_xusb: MSG_ENABLED not acknowledged within 250 ms");
                }
            }
        }
    });
}

} // namespace tegra_xusb

#endif // STLX_PLATFORM_JETSON_NANO
