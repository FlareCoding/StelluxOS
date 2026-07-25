#include "drivers/gpu/nvidia/nv_vbios.h"
#include "mm/vmm.h"
#include "mm/kva.h"
#include "mm/paging_types.h"
#include "hw/mmio.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"

// All constants below are transcribed from open-gpu-kernel-modules @535.183.01:
//   - NV_PROM_DATA(i)=0x300000+i           (turing/tu102/dev_ext_devices.h:27)
//   - PCI exp-ROM / PCIR / NPDE offsets     (inc/kernel/platform/pci_exp_table.h)
//   - IFR FIXED0/1/2                         (turing/tu102/dev_bus.h:29-36)
//   - extract algorithm                      (kernel_gsp_vbios_tu102.c)
namespace nvidia {

// NV_PROM aperture base within BAR0 (byte-addressable expansion ROM window).
constexpr uint32_t NV_PROM_DATA_BASE = 0x00300000;

// PCI expansion-ROM header (16-bit signature @ off 0).
constexpr uint16_t PCI_EXP_ROM_SIGNATURE     = 0xAA55;
constexpr uint16_t PCI_EXP_ROM_SIGNATURE_NV  = 0x4E56;
constexpr uint16_t PCI_EXP_ROM_SIGNATURE_NV2 = 0xBB77;
constexpr uint32_t OFF_EXP_ROM_SIG                 = 0x0;
constexpr uint32_t OFF_EXP_ROM_PCI_DATA_STRUCT_PTR = 0x18;

// PCI data structure ("PCIR" / "NPDS" / "RGIS"), pointed to by the exp-ROM header.
constexpr uint32_t PCI_DATA_STRUCT_SIGNATURE     = 0x52494350; // "PCIR"
constexpr uint32_t PCI_DATA_STRUCT_SIGNATURE_NV  = 0x5344504E; // "NPDS"
constexpr uint32_t PCI_DATA_STRUCT_SIGNATURE_NV2 = 0x53494752; // "RGIS"
constexpr uint32_t OFF_PCIR_SIG        = 0x0;
constexpr uint32_t OFF_PCIR_LEN        = 0xA;
constexpr uint32_t OFF_PCIR_IMAGE_LEN  = 0x10;
constexpr uint32_t OFF_PCIR_CODE_TYPE  = 0x14;
constexpr uint32_t OFF_PCIR_LAST_IMAGE = 0x15;
constexpr uint8_t  PCI_LAST_IMAGE      = 0x80; // NVBIT(7)
constexpr uint32_t PCI_ROM_BLOCK_SIZE  = 512;

// NVIDIA PCI data extension ("NPDE").
constexpr uint32_t NV_PCI_DATA_EXT_SIG = 0x4544504E; // "NPDE"
constexpr uint16_t NV_PCI_DATA_EXT_REV_10 = 0x100;
constexpr uint16_t NV_PCI_DATA_EXT_REV_11 = 0x101;
constexpr uint32_t OFF_NPDE_SIG          = 0x0;
constexpr uint32_t OFF_NPDE_REV          = 0x4;
constexpr uint32_t OFF_NPDE_LEN          = 0x6;
constexpr uint32_t OFF_NPDE_SUBIMAGE_LEN = 0x8;
constexpr uint32_t OFF_NPDE_LAST_IMAGE   = 0xA;

// IFR (leading firmware region) header — read when offset 0 isn't a ROM sig.
constexpr uint32_t NV_PBUS_IFR_FMT_FIXED0           = 0x0;
constexpr uint32_t NV_PBUS_IFR_FMT_FIXED1           = 0x4;
constexpr uint32_t NV_PBUS_IFR_FMT_FIXED2           = 0x8;
constexpr uint32_t IFR_FIXED0_SIGNATURE_VALUE       = 0x4947564E; // "NVGI"
constexpr uint32_t NV_ROM_DIRECTORY_IDENTIFIER      = 0x44524652; // "RFRD"

// Expansion-ROM code types.
constexpr uint8_t  CODE_TYPE_VBIOS_BASE = 0x00;
constexpr uint8_t  CODE_TYPE_VBIOS_EXT  = 0xE0;

constexpr uint32_t VBIOS_MAX_SIZE = 0x100000; // 1 MB (s_getBaseBiosMaxSize_TU102)

// ---- raw PROM reads (caller is already elevated; reads land in BAR0) -------
static inline uint8_t prom_rd8(uintptr_t bar0_va, uint32_t off) {
    return mmio::read8(bar0_va + NV_PROM_DATA_BASE + off);
}
static inline uint16_t prom_rd16(uintptr_t bar0_va, uint32_t off) {
    return static_cast<uint16_t>(prom_rd8(bar0_va, off)) |
           static_cast<uint16_t>(static_cast<uint16_t>(prom_rd8(bar0_va, off + 1)) << 8);
}
static inline uint32_t prom_rd32(uintptr_t bar0_va, uint32_t off) {
    return static_cast<uint32_t>(prom_rd8(bar0_va, off)) |
           (static_cast<uint32_t>(prom_rd8(bar0_va, off + 1)) << 8) |
           (static_cast<uint32_t>(prom_rd8(bar0_va, off + 2)) << 16) |
           (static_cast<uint32_t>(prom_rd8(bar0_va, off + 3)) << 24);
}

static bool is_valid_rom_sig(uint16_t s) {
    return s == PCI_EXP_ROM_SIGNATURE || s == PCI_EXP_ROM_SIGNATURE_NV ||
           s == PCI_EXP_ROM_SIGNATURE_NV2;
}
static bool is_valid_data_sig(uint32_t s) {
    return s == PCI_DATA_STRUCT_SIGNATURE || s == PCI_DATA_STRUCT_SIGNATURE_NV ||
           s == PCI_DATA_STRUCT_SIGNATURE_NV2;
}

// Mirror of s_romImgFindPciHeader_TU102: when the PROM starts with an IFR region
// (not a ROM signature), compute the offset where the real PCI ROM image begins.
static int32_t find_pci_header(uintptr_t bar0_va, uint32_t* pImageOffset) {
    const uint32_t fixed0 = prom_rd32(bar0_va, NV_PBUS_IFR_FMT_FIXED0);
    const uint32_t fixed1 = prom_rd32(bar0_va, NV_PBUS_IFR_FMT_FIXED1);
    const uint32_t fixed2 = prom_rd32(bar0_va, NV_PBUS_IFR_FMT_FIXED2);
    uint32_t imageOffset = 0;

    if (fixed0 == IFR_FIXED0_SIGNATURE_VALUE) {
        const uint32_t ifrVersion = (fixed1 >> 8) & 0xFF; // VERSIONSW 15:8
        log::info("nvidia: vbios: IFR signature found, version=0x%02x", ifrVersion);
        if (ifrVersion == 0x01 || ifrVersion == 0x02) {
            const uint32_t extendedOffset = (fixed1 >> 16) & 0x7FFF; // FIXED_DATA_SIZE 30:16
            imageOffset = prom_rd32(bar0_va, extendedOffset + 4);
        } else if (ifrVersion == 0x03) {
            const uint32_t ifrTotalDataSize   = fixed2 & 0xFFFFF; // TOTAL_DATA_SIZE 19:0
            const uint32_t flashStatusOffset  = prom_rd32(bar0_va, ifrTotalDataSize);
            const uint32_t romDirectoryOffset = flashStatusOffset + 4096;
            const uint32_t romDirectorySig    = prom_rd32(bar0_va, romDirectoryOffset);
            if (romDirectorySig != NV_ROM_DIRECTORY_IDENTIFIER) {
                log::error("nvidia: vbios: ROM directory not found (sig=0x%08x)", romDirectorySig);
                return ERR_VB_IFR;
            }
            imageOffset = prom_rd32(bar0_va, romDirectoryOffset + 8);
        } else {
            log::error("nvidia: vbios: unsupported IFR version 0x%02x", ifrVersion);
            return ERR_VB_IFR;
        }
    } else {
        log::warn("nvidia: vbios: no IFR signature (FIXED0=0x%08x) and no ROM sig at 0", fixed0);
    }

    if ((imageOffset & 0x3) != 0) {
        log::error("nvidia: vbios: misaligned image offset 0x%x", imageOffset);
        return ERR_VB_IFR;
    }
    *pImageOffset = imageOffset;
    log::info("nvidia: vbios: PCI ROM image begins at PROM offset 0x%x", imageOffset);
    return VB_OK;
}

// Mirror of s_locateExpansionRoms: walk the PCIR (+ NPDE) image chain to compute
// the total VBIOS size and the base->ext expansion-ROM delta.
static int32_t locate_expansion_roms(uintptr_t bar0_va, uint32_t pciOffset,
                                     uint32_t* pBiosSize, uint32_t* pExtRomOff) {
    uint32_t currBlock = pciOffset;
    uint32_t extRomOffset = 0, baseRomSize = 0;
    uint32_t blockOffset = 0, blockSize = 0;

    for (int guard = 0; guard < 64; guard++) {
        const uint32_t pciBlck  = prom_rd16(bar0_va, currBlock + OFF_EXP_ROM_PCI_DATA_STRUCT_PTR);
        const uint32_t pcirBase = currBlock + pciBlck;
        const uint32_t pcirSig  = prom_rd32(bar0_va, pcirBase + OFF_PCIR_SIG);
        if (!is_valid_data_sig(pcirSig)) {
            log::error("nvidia: vbios: invalid PCIR sig 0x%08x at 0x%x", pcirSig, pcirBase);
            return ERR_VB_ROMS;
        }

        bool     bIsLastImage = (prom_rd8(bar0_va, pcirBase + OFF_PCIR_LAST_IMAGE) & PCI_LAST_IMAGE) != 0;
        const uint32_t imgLen = prom_rd16(bar0_va, pcirBase + OFF_PCIR_IMAGE_LEN);
        uint32_t subImgLen    = imgLen;

        // Optional NVIDIA PCI Data Extension (NPDE), 16-byte aligned after PCIR.
        const uint16_t pcirLen = prom_rd16(bar0_va, pcirBase + OFF_PCIR_LEN);
        const uint32_t npdeAt  = (pcirBase + pcirLen + 0xF) & ~0xFu;
        const uint32_t npdeSig = prom_rd32(bar0_va, npdeAt + OFF_NPDE_SIG);
        if (npdeSig == NV_PCI_DATA_EXT_SIG) {
            const uint16_t npdeRev = prom_rd16(bar0_va, npdeAt + OFF_NPDE_REV);
            if (npdeRev == NV_PCI_DATA_EXT_REV_10 || npdeRev == NV_PCI_DATA_EXT_REV_11) {
                const uint16_t npdeLen = prom_rd16(bar0_va, npdeAt + OFF_NPDE_LEN);
                subImgLen = prom_rd16(bar0_va, npdeAt + OFF_NPDE_SUBIMAGE_LEN);
                if (OFF_NPDE_LAST_IMAGE + 1 <= npdeLen) {
                    bIsLastImage = (prom_rd8(bar0_va, npdeAt + OFF_NPDE_LAST_IMAGE) & PCI_LAST_IMAGE) != 0;
                } else if (subImgLen < imgLen) {
                    bIsLastImage = false;
                }
            }
        }

        const uint8_t type = prom_rd8(bar0_va, pcirBase + OFF_PCIR_CODE_TYPE);
        blockOffset = currBlock - pciOffset;
        blockSize   = subImgLen * PCI_ROM_BLOCK_SIZE;

        log::info("nvidia: vbios:   block@0x%x pcir@0x%x type=0x%02x imgLen=%u subLen=%u last=%u",
                  blockOffset, pcirBase, type, imgLen, subImgLen, bIsLastImage ? 1u : 0u);

        if (extRomOffset == 0 && type == CODE_TYPE_VBIOS_EXT) {
            extRomOffset = blockOffset;
        } else if (baseRomSize == 0 && type == CODE_TYPE_VBIOS_BASE) {
            baseRomSize = blockSize;
        }

        if (bIsLastImage) break;
        if (subImgLen == 0) {
            log::error("nvidia: vbios: zero-length image block - aborting walk");
            return ERR_VB_ROMS;
        }
        currBlock += subImgLen * PCI_ROM_BLOCK_SIZE;
    }

    *pBiosSize = blockOffset + blockSize;
    *pExtRomOff = (extRomOffset > 0 && baseRomSize > 0) ? (extRomOffset - baseRomSize) : 0;
    return VB_OK;
}

// Runs already-elevated (see vbios_extract_from_rom).
static int32_t vbios_extract_locked(uintptr_t bar0_va, vbios_image& out) {
    uint32_t pciOffset = 0;

    uint16_t romSig = prom_rd16(bar0_va, OFF_EXP_ROM_SIG);
    if (!is_valid_rom_sig(romSig)) {
        // Leading IFR region: find where the real PCI ROM image starts.
        const int32_t r = find_pci_header(bar0_va, &pciOffset);
        if (r != VB_OK) return r;
        romSig = prom_rd16(bar0_va, pciOffset + OFF_EXP_ROM_SIG);
    }
    if (!is_valid_rom_sig(romSig)) {
        log::error("nvidia: vbios: no valid ROM signature (got 0x%04x at 0x%x)", romSig, pciOffset);
        return ERR_VB_NOSIG;
    }
    log::info("nvidia: vbios: ROM signature 0x%04x at PROM offset 0x%x", romSig, pciOffset);

    uint32_t biosSize = 0, extRomOff = 0;
    const int32_t r = locate_expansion_roms(bar0_va, pciOffset, &biosSize, &extRomOff);
    if (r != VB_OK) return r;

    log::info("nvidia: vbios: total size=%u bytes (0x%x), expansionRomOffset=0x%x",
              biosSize, biosSize, extRomOff);
    if (biosSize == 0 || biosSize > VBIOS_MAX_SIZE) {
        log::error("nvidia: vbios: size out of range (%u)", biosSize);
        return ERR_VB_SIZE;
    }

    // Allocate the image buffer (page-backed; up to 1 MB) and copy it out.
    const size_t pages = (biosSize + paging::PAGE_SIZE_4KB - 1) / paging::PAGE_SIZE_4KB;
    uintptr_t buf_va = 0;
    // PAGE_USER: the image is parsed later from the ring-3 driver task (FWSEC extraction), so the
    // buffer must be user-accessible (this alloc already runs elevated via vbios_extract_from_rom).
    if (vmm::alloc(pages, paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_USER, vmm::ALLOC_ZERO,
                   kva::tag::generic, buf_va) != vmm::OK) {
        log::error("nvidia: vbios: failed to allocate %lu pages for image", (unsigned long)pages);
        return ERR_VB_MEM;
    }
    uint8_t* img = reinterpret_cast<uint8_t*>(buf_va);

    const uint32_t aligned = biosSize & ~0x3u;
    for (uint32_t i = 0; i < aligned; i += 4) {
        *reinterpret_cast<uint32_t*>(img + i) =
            mmio::read32(bar0_va + NV_PROM_DATA_BASE + pciOffset + i);
    }
    for (uint32_t i = aligned; i < biosSize; i++) {
        img[i] = prom_rd8(bar0_va, pciOffset + i);
    }

    log::info("nvidia: vbios: image[0..15] = %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
              img[0], img[1], img[2], img[3], img[4], img[5], img[6], img[7],
              img[8], img[9], img[10], img[11], img[12], img[13], img[14], img[15]);

    out.image = img;
    out.size = biosSize;
    out.ext_rom_off = extRomOff;
    return VB_OK;
}

int32_t vbios_extract_from_rom(uintptr_t bar0_va, vbios_image& out) {
    out.image = nullptr;
    out.size = 0;
    out.ext_rom_off = 0;
    int32_t rc = ERR_VB_NOSIG;
    RUN_ELEVATED(rc = vbios_extract_locked(bar0_va, out));
    return rc;
}

void vbios_free(vbios_image& img) {
    if (img.image) {
        RUN_ELEVATED(vmm::free(reinterpret_cast<uintptr_t>(img.image)));
    }
    img.image = nullptr;
    img.size = 0;
    img.ext_rom_off = 0;
}

} // namespace nvidia
