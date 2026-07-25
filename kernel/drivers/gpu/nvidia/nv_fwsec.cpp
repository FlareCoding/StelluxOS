#include "drivers/gpu/nvidia/nv_fwsec.h"
#include "common/logging.h"

// Constants transcribed from open-gpu-kernel-modules @535.183.01
// (kernel_gsp_fwsec.c). See nv_fwsec.h for the algorithm reference.
namespace nvidia {

// BIT header
constexpr uint16_t BIT_HEADER_ID          = 0xB8FF;
constexpr uint32_t BIT_HEADER_SIGNATURE   = 0x00544942; // "BIT\0"
constexpr uint32_t BIT_HEADER_SIZE_OFFSET = 8;          // HeaderSize byte offset

// BIT tokens of interest
constexpr uint8_t BIT_TOKEN_BIOSDATA    = 0x42;
constexpr uint8_t BIT_TOKEN_FALCON_DATA = 0x70;

// Falcon ucode table header version + entry application IDs
constexpr uint8_t FALCON_UCODE_TABLE_HDR_V1_VERSION = 1;
constexpr uint8_t APPID_FIRMWARE_SEC_LIC = 0x05;
constexpr uint8_t APPID_FWSEC_DBG        = 0x45;
constexpr uint8_t APPID_FWSEC_PROD       = 0x85;

// ---- bounds-checked little-endian readers over the VBIOS image -------------
static uint8_t vb_rd8(const vbios_image& vb, uint32_t off) {
    return (off < vb.size) ? vb.image[off] : 0;
}
static uint16_t vb_rd16(const vbios_image& vb, uint32_t off) {
    return static_cast<uint16_t>(vb_rd8(vb, off)) |
           static_cast<uint16_t>(static_cast<uint16_t>(vb_rd8(vb, off + 1)) << 8);
}
static uint32_t vb_rd32(const vbios_image& vb, uint32_t off) {
    return static_cast<uint32_t>(vb_rd8(vb, off)) |
           (static_cast<uint32_t>(vb_rd8(vb, off + 1)) << 8) |
           (static_cast<uint32_t>(vb_rd8(vb, off + 2)) << 16) |
           (static_cast<uint32_t>(vb_rd8(vb, off + 3)) << 24);
}

// Mirror of s_vbiosFindBitHeader: scan for 0xB8FF + "BIT\0" and verify the
// header checksum (sum of HeaderSize bytes == 0 mod 256).
static bool find_bit_header(const vbios_image& vb, uint32_t* pBitAddr) {
    if (vb.size < 4) return false;
    for (uint32_t addr = 0; addr + 4 <= vb.size; addr++) {
        if (vb_rd16(vb, addr) != BIT_HEADER_ID) continue;
        if (vb_rd32(vb, addr + 2) != BIT_HEADER_SIGNATURE) continue;

        const uint32_t headerSize = vb_rd8(vb, addr + BIT_HEADER_SIZE_OFFSET);
        uint32_t checksum = 0;
        for (uint32_t j = 0; j < headerSize; j++) {
            checksum += vb_rd8(vb, addr + j);
        }
        if ((checksum & 0xFF) == 0) {
            *pBitAddr = addr;
            return true;
        }
    }
    return false;
}

int32_t fwsec_parse(const vbios_image& vb, bool use_debug, fwsec_info& out) {
    out.vbios_version = 0;
    out.bit_addr = 0;
    out.ucode_table_ptr = 0;
    out.app_id = 0;
    out.desc_offset = 0;
    out.desc_version = 0;
    out.desc_size = 0;

    // 1) Locate the BIT header.
    uint32_t bitAddr = 0;
    if (!find_bit_header(vb, &bitAddr)) {
        log::error("nvidia: fwsec: BIT header (0xB8FF \"BIT\") not found");
        return ERR_FWSEC_NOBIT;
    }
    out.bit_addr = bitAddr;

    const uint8_t  hdrSize    = vb_rd8(vb, bitAddr + 8);
    const uint8_t  tokSize    = vb_rd8(vb, bitAddr + 9);
    const uint8_t  tokEntries = vb_rd8(vb, bitAddr + 10);
    log::info("nvidia: fwsec: BIT header @0x%x hdrSize=%u tokSize=%u tokens=%u",
              bitAddr, hdrSize, tokSize, tokEntries);
    if (tokSize == 0) {
        log::error("nvidia: fwsec: zero token size");
        return ERR_FWSEC_NOBIT;
    }

    // 2) Walk BIT tokens: capture BIOSDATA (version) + FALCON_DATA (ucode table).
    bool     foundFalcon = false;
    uint32_t falconTablePtr = 0;
    for (uint32_t t = 0; t < tokEntries; t++) {
        const uint32_t tokOff   = bitAddr + hdrSize + t * tokSize;
        const uint8_t  tokId    = vb_rd8(vb, tokOff + 0);
        const uint8_t  dataVer  = vb_rd8(vb, tokOff + 1);
        const uint16_t dataSize = vb_rd16(vb, tokOff + 2);
        const uint16_t dataPtr  = vb_rd16(vb, tokOff + 4);

        if (tokId == BIT_TOKEN_BIOSDATA && (dataVer == 1 || dataVer == 2) && dataSize > 5) {
            const uint32_t ver = vb_rd32(vb, dataPtr);       // BINVER.Version (1d)
            const uint8_t  oem = vb_rd8(vb, dataPtr + 4);    // BINVER.OemVersion (1b)
            out.vbios_version = (static_cast<uint64_t>(ver) << 8) | oem;
        }

        if (tokId == BIT_TOKEN_FALCON_DATA && dataVer == 2 && dataSize >= 4) {
            falconTablePtr = vb_rd32(vb, dataPtr);           // FALCON_DATA_V2.FalconUcodeTablePtr
            foundFalcon = true;
            log::info("nvidia: fwsec: FALCON_DATA token @0x%x -> FalconUcodeTablePtr=0x%x",
                      tokOff, falconTablePtr);
        }
    }

    if (out.vbios_version != 0) {
        const uint64_t v = out.vbios_version;
        log::info("nvidia: fwsec: VBIOS version %02x.%02x.%02x.%02x.%02x",
                  static_cast<unsigned>((v >> 32) & 0xFF), static_cast<unsigned>((v >> 24) & 0xFF),
                  static_cast<unsigned>((v >> 16) & 0xFF), static_cast<unsigned>((v >> 8) & 0xFF),
                  static_cast<unsigned>(v & 0xFF));
    }
    if (!foundFalcon) {
        log::error("nvidia: fwsec: no FALCON_DATA (0x70) token found");
        return ERR_FWSEC_NOFLCN;
    }
    out.ucode_table_ptr = falconTablePtr;

    // 3) Falcon ucode table header - addressed at (expansionRomOffset + ptr).
    const uint32_t tableBase = vb.ext_rom_off + falconTablePtr;
    const uint8_t  tblVer      = vb_rd8(vb, tableBase + 0);
    const uint8_t  tblHdrSize  = vb_rd8(vb, tableBase + 1);
    const uint8_t  tblEntSize  = vb_rd8(vb, tableBase + 2);
    const uint8_t  tblEntCount = vb_rd8(vb, tableBase + 3);
    log::info("nvidia: fwsec: ucode table @0x%x (ext_off 0x%x + 0x%x) ver=%u hdrSize=%u entSize=%u entCount=%u",
              tableBase, vb.ext_rom_off, falconTablePtr, tblVer, tblHdrSize, tblEntSize, tblEntCount);
    if (tblVer != FALCON_UCODE_TABLE_HDR_V1_VERSION || tblHdrSize < 6 || tblEntSize < 6) {
        log::error("nvidia: fwsec: unexpected ucode table geometry");
        return ERR_FWSEC_TABLE;
    }

    // 4) Walk entries to find the FWSEC entry; read its descriptor header.
    for (uint32_t e = 0; e < tblEntCount; e++) {
        const uint32_t entOff  = tableBase + tblHdrSize + e * tblEntSize;
        const uint8_t  appId   = vb_rd8(vb, entOff + 0);
        const uint8_t  target  = vb_rd8(vb, entOff + 1);
        const uint32_t descPtr = vb_rd32(vb, entOff + 2);

        const bool isFwsec =
            (appId == APPID_FIRMWARE_SEC_LIC) ||
            (use_debug ? (appId == APPID_FWSEC_DBG) : (appId == APPID_FWSEC_PROD));

        log::info("nvidia: fwsec:   entry[%u] appId=0x%02x target=0x%02x descPtr=0x%x%s",
                  e, appId, target, descPtr, isFwsec ? "  <= FWSEC" : "");
        if (!isFwsec) continue;

        // Descriptor header at (expansionRomOffset + DescPtr).
        const uint32_t descOff = vb.ext_rom_off + descPtr;
        const uint32_t vDesc   = vb_rd32(vb, descOff);
        if ((vDesc & 0x1) == 0) { // FLAGS_VERSION 0:0 == UNAVAILABLE
            log::warn("nvidia: fwsec:   entry[%u] descriptor version unavailable, skipping", e);
            continue;
        }
        const uint8_t  descVer = static_cast<uint8_t>((vDesc >> 8) & 0xFF);  // VDESC_VERSION 15:8
        const uint32_t descSz  = (vDesc >> 16) & 0xFFFF;                     // VDESC_SIZE 31:16

        out.app_id = appId;
        out.desc_offset = descOff;
        out.desc_version = descVer;
        out.desc_size = descSz;

        log::info("nvidia: fwsec: FWSEC descriptor @0x%x version=V%u size=%u (appId=0x%02x)",
                  descOff, descVer, descSz, appId);

        if (descVer != 3) {
            log::warn("nvidia: fwsec: expected V3 descriptor on GA102, got V%u", descVer);
            return ERR_FWSEC_ENTRY;
        }

        // Parse the FALCON_UCODE_DESC_V3 body (Hdr is the 1 dword @ descOff+0).
        out.stored_size        = vb_rd32(vb, descOff + 4);
        out.pkc_data_offset    = vb_rd32(vb, descOff + 8);
        out.interface_offset   = vb_rd32(vb, descOff + 12);
        out.imem_phys_base     = vb_rd32(vb, descOff + 16);
        out.imem_load_size     = vb_rd32(vb, descOff + 20);
        out.imem_virt_base     = vb_rd32(vb, descOff + 24);
        out.dmem_phys_base     = vb_rd32(vb, descOff + 28);
        out.dmem_load_size     = vb_rd32(vb, descOff + 32);
        out.engine_id_mask     = vb_rd16(vb, descOff + 36);
        out.ucode_id           = vb_rd8(vb, descOff + 38);
        out.signature_count    = vb_rd8(vb, descOff + 39);
        out.signature_versions = vb_rd16(vb, descOff + 40);

        log::info("nvidia: fwsec:   IMEM phys=0x%x load=%u virt=0x%x | DMEM phys=0x%x load=%u",
                  out.imem_phys_base, out.imem_load_size, out.imem_virt_base,
                  out.dmem_phys_base, out.dmem_load_size);
        log::info("nvidia: fwsec:   storedSize=%u interfaceOff=0x%x pkcDataOff=0x%x",
                  out.stored_size, out.interface_offset, out.pkc_data_offset);
        log::info("nvidia: fwsec:   engineIdMask=0x%x ucodeId=%u sigCount=%u sigVersions=0x%x",
                  out.engine_id_mask, out.ucode_id, out.signature_count, out.signature_versions);
        return FWSEC_OK;
    }

    log::error("nvidia: fwsec: no FWSEC entry found in ucode table");
    return ERR_FWSEC_ENTRY;
}

// V3 descriptor header size; the ucode payload + signatures follow the desc.
constexpr uint32_t FALCON_UCODE_DESC_V3_SIZE_44 = 44;

int32_t fwsec_load_ucode(const vbios_image& vb, const fwsec_info& fi, fwsec_ucode& out) {
    out.image = dma_buffer{};
    out.total_size = 0;
    out.imem_size = 0;
    out.dmem_size = 0;

    // The ucode (code then data) is stored immediately after the descriptor+sigs,
    // length ALIGN_UP(StoredSize, 256). (s_vbiosFillFlcnUcodeFromDescV3)
    const uint32_t total    = (fi.stored_size + 255u) & ~255u;
    const uint32_t imageOff = fi.desc_offset + fi.desc_size;
    if (total == 0 || (static_cast<uint64_t>(imageOff) + total) > vb.size) {
        log::error("nvidia: fwsec: ucode image [0x%x +%u] out of VBIOS bounds (size %u)",
                   imageOff, total, vb.size);
        return ERR_FWSEC_ENTRY;
    }

    dma_buffer buf;
    if (dma_alloc(total, /*uncached=*/true, buf) != MEM_OK) {
        return ERR_FWSEC_ENTRY;
    }

    uint8_t* dst = reinterpret_cast<uint8_t*>(buf.cpu_va);
    const uint8_t* src = vb.image + imageOff;
    for (uint32_t i = 0; i < total; i++) {
        dst[i] = src[i];
    }

    out.image = buf;
    out.total_size = total;
    out.imem_size = fi.imem_load_size;
    out.dmem_size = fi.dmem_load_size;

    const uint32_t dataOff = out.imem_size;
    log::info("nvidia: fwsec: ucode staged @phys=0x%lx va=0x%lx total=%u (code=%u, data=%u @0x%x)",
              static_cast<unsigned long>(buf.phys), static_cast<unsigned long>(buf.cpu_va),
              total, out.imem_size, out.dmem_size, dataOff);
    log::info("nvidia: fwsec:   code[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x",
              dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7]);
    if (static_cast<uint64_t>(dataOff) + 8 <= total) {
        log::info("nvidia: fwsec:   data[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x",
                  dst[dataOff + 0], dst[dataOff + 1], dst[dataOff + 2], dst[dataOff + 3],
                  dst[dataOff + 4], dst[dataOff + 5], dst[dataOff + 6], dst[dataOff + 7]);
    }
    log::info("nvidia: fwsec:   signatures in VBIOS @0x%x count=%u size=%u total=%u",
              fi.desc_offset + FALCON_UCODE_DESC_V3_SIZE_44, fi.signature_count, 384u,
              fi.signature_count * 384u);
    return FWSEC_OK;
}

void fwsec_free_ucode(fwsec_ucode& out) {
    dma_free(out.image);
    out.total_size = 0;
    out.imem_size = 0;
    out.dmem_size = 0;
}

} // namespace nvidia
