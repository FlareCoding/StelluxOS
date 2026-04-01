#include "drivers/graphics/nvidia/nv_firmware.h"
#include "drivers/graphics/nvidia/nv_gpu.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "common/logging.h"
#include "common/string.h"

namespace nv {

// ============================================================================
// Helper: Read a little-endian value from buffer
// ============================================================================

static uint16_t rd16(const uint8_t* buf, uint32_t off) {
    return static_cast<uint16_t>(buf[off]) |
           (static_cast<uint16_t>(buf[off + 1]) << 8);
}

static uint32_t rd32(const uint8_t* buf, uint32_t off) {
    return static_cast<uint32_t>(buf[off]) |
           (static_cast<uint32_t>(buf[off + 1]) << 8) |
           (static_cast<uint32_t>(buf[off + 2]) << 16) |
           (static_cast<uint32_t>(buf[off + 3]) << 24);
}

// rd64 will be needed in later phases (WPR metadata, etc.)
// static uint64_t rd64(const uint8_t* buf, uint32_t off) {
//     return static_cast<uint64_t>(rd32(buf, off)) |
//            (static_cast<uint64_t>(rd32(buf, off + 4)) << 32);
// }

// ============================================================================
// File Loading
// ============================================================================

int32_t firmware_load_file(const char* path, uint8_t*& out_data,
                           uint32_t& out_size, uint32_t max_size) {
    log::info("nvidia: fw: loading %s", path);

    // Get file size
    fs::vattr attr;
    int32_t err = fs::stat(path, &attr);
    if (err != fs::OK) {
        log::error("nvidia: fw: stat(%s) failed: %d", path, err);
        return ERR_NOT_FOUND;
    }

    uint64_t file_size = attr.size;
    if (file_size == 0) {
        log::error("nvidia: fw: %s is empty", path);
        return ERR_INVALID;
    }
    if (file_size > max_size) {
        log::error("nvidia: fw: %s too large (%lu > %u)", path, file_size, max_size);
        return ERR_INVALID;
    }

    // Allocate buffer
    uint8_t* buf = static_cast<uint8_t*>(heap::kzalloc(static_cast<size_t>(file_size)));
    if (!buf) {
        log::error("nvidia: fw: failed to allocate %lu bytes for %s", file_size, path);
        return ERR_NOT_FOUND;
    }

    // Open and read
    int32_t open_err = 0;
    fs::file* f = fs::open(path, fs::O_RDONLY, &open_err);
    if (!f) {
        log::error("nvidia: fw: open(%s) failed: %d", path, open_err);
        heap::kfree(buf);
        return ERR_NOT_FOUND;
    }

    ssize_t bytes_read = fs::read(f, buf, static_cast<size_t>(file_size));
    fs::close(f);

    if (bytes_read < 0 || static_cast<uint64_t>(bytes_read) != file_size) {
        log::error("nvidia: fw: read(%s) returned %ld (expected %lu)",
                   path, bytes_read, file_size);
        heap::kfree(buf);
        return ERR_IO;
    }

    out_data = buf;
    out_size = static_cast<uint32_t>(file_size);
    log::info("nvidia: fw: loaded %s (%u bytes)", path, out_size);
    return OK;
}

// ============================================================================
// BinHdr Parsing
// ============================================================================

int32_t firmware_parse_binhdr(const uint8_t* data, uint32_t size, bin_hdr& out) {
    if (size < sizeof(bin_hdr)) {
        log::error("nvidia: fw: file too small for BinHdr (%u < %lu)", size, sizeof(bin_hdr));
        return ERR_INVALID;
    }

    string::memcpy(&out, data, sizeof(bin_hdr));

    if (out.bin_magic != FW_BIN_MAGIC) {
        log::error("nvidia: fw: BinHdr magic mismatch: 0x%04x (expected 0x%04x)",
                   out.bin_magic, FW_BIN_MAGIC);
        return ERR_INVALID;
    }

    // Validate offsets are within bounds
    if (out.header_offset >= size) {
        log::error("nvidia: fw: header_offset 0x%x out of bounds (size=%u)",
                   out.header_offset, size);
        return ERR_INVALID;
    }
    if (out.data_offset >= size || out.data_offset + out.data_size > size) {
        log::error("nvidia: fw: data range [0x%x, 0x%x) out of bounds (size=%u)",
                   out.data_offset, out.data_offset + out.data_size, size);
        return ERR_INVALID;
    }

    log::info("nvidia: fw: BinHdr: magic=0x%04x ver=%u hdr_off=0x%x data_off=0x%x data_size=0x%x",
              out.bin_magic, out.bin_ver, out.header_offset, out.data_offset, out.data_size);
    return OK;
}

// ============================================================================
// Booter Firmware Parsing (booter_load, booter_unload)
// ============================================================================

int32_t firmware_parse_booter(const uint8_t* data, uint32_t size, booter_fw& out) {
    // Step 1: Parse BinHdr
    int32_t rc = firmware_parse_binhdr(data, size, out.hdr);
    if (rc != OK) return rc;

    // Step 2: Parse HsHeaderV2 at header_offset
    uint32_t hs_off = out.hdr.header_offset;
    if (hs_off + sizeof(hs_header_v2) > size) {
        log::error("nvidia: fw: HsHeaderV2 out of bounds at offset 0x%x", hs_off);
        return ERR_INVALID;
    }
    string::memcpy(&out.hs_hdr, data + hs_off, sizeof(hs_header_v2));

    log::info("nvidia: fw: HsHeaderV2: sig_prod_off=0x%x sig_prod_size=0x%x "
              "load_hdr_off=0x%x load_hdr_size=0x%x",
              out.hs_hdr.sig_prod_offset, out.hs_hdr.sig_prod_size,
              out.hs_hdr.header_offset, out.hs_hdr.header_size);
    log::info("nvidia: fw: HsHeaderV2: patch_loc=0x%x patch_sig=0x%x num_sig=0x%x "
              "meta_off=0x%x meta_size=0x%x",
              out.hs_hdr.patch_loc, out.hs_hdr.patch_sig, out.hs_hdr.num_sig,
              out.hs_hdr.meta_data_offset, out.hs_hdr.meta_data_size);

    // Step 3: Parse HsLoadHeaderV2 at hs_hdr.header_offset
    uint32_t load_off = out.hs_hdr.header_offset;
    if (load_off + sizeof(hs_load_header_v2) > size) {
        log::error("nvidia: fw: HsLoadHeaderV2 out of bounds at offset 0x%x", load_off);
        return ERR_INVALID;
    }
    string::memcpy(&out.load_hdr, data + load_off, sizeof(hs_load_header_v2));

    log::info("nvidia: fw: HsLoadHeaderV2: code_off=0x%x code_size=0x%x "
              "data_off=0x%x data_size=0x%x num_apps=%u",
              out.load_hdr.os_code_offset, out.load_hdr.os_code_size,
              out.load_hdr.os_data_offset, out.load_hdr.os_data_size,
              out.load_hdr.num_apps);

    // Step 4: Parse first app entry (immediately after HsLoadHeaderV2)
    if (out.load_hdr.num_apps == 0) {
        log::error("nvidia: fw: booter has 0 apps");
        return ERR_INVALID;
    }

    uint32_t app_off = load_off + sizeof(hs_load_header_v2);
    if (app_off + sizeof(hs_load_app_v2) > size) {
        log::error("nvidia: fw: app entry out of bounds at offset 0x%x", app_off);
        return ERR_INVALID;
    }
    string::memcpy(&out.app, data + app_off, sizeof(hs_load_app_v2));

    log::info("nvidia: fw: App[0]: code_off=0x%x code_size=0x%x "
              "data_off=0x%x data_size=0x%x",
              out.app.offset, out.app.size,
              out.app.data_offset, out.app.data_size);

    // Step 5: Read signature metadata (fuse version, engine ID, ucode ID)
    string::memset(&out.sig_meta, 0, sizeof(hs_sig_params));
    if (out.hs_hdr.meta_data_offset + sizeof(hs_sig_params) <= size) {
        string::memcpy(&out.sig_meta, data + out.hs_hdr.meta_data_offset, sizeof(hs_sig_params));
        log::info("nvidia: fw: SigParams: fuse_ver=%u engine_id_mask=0x%x ucode_id=%u",
                  out.sig_meta.fuse_ver, out.sig_meta.engine_id_mask, out.sig_meta.ucode_id);
    }

    // Step 6: Dereference indirect values
    // patch_loc, patch_sig, num_sig are OFFSETS to u32 values in the data, not direct values
    out.patch_loc_val = 0;
    out.patch_sig_val = 0;
    out.num_sig_val = 0;
    out.per_sig_size = 0;

    if (out.hs_hdr.patch_loc + 4 <= size) {
        out.patch_loc_val = rd32(data, out.hs_hdr.patch_loc);
        log::info("nvidia: fw: patch_loc (indirect at 0x%x): DMEM offset = 0x%x",
                  out.hs_hdr.patch_loc, out.patch_loc_val);
    }
    if (out.hs_hdr.patch_sig + 4 <= size) {
        out.patch_sig_val = rd32(data, out.hs_hdr.patch_sig);
        log::info("nvidia: fw: patch_sig (indirect at 0x%x): sig base index = 0x%x",
                  out.hs_hdr.patch_sig, out.patch_sig_val);
    }
    if (out.hs_hdr.num_sig + 4 <= size) {
        out.num_sig_val = rd32(data, out.hs_hdr.num_sig);
        log::info("nvidia: fw: num_sig (indirect at 0x%x): count = %u",
                  out.hs_hdr.num_sig, out.num_sig_val);
    }

    // Calculate per-signature size (sig_prod_size / sig_count)
    if (out.num_sig_val > 0) {
        out.per_sig_size = out.hs_hdr.sig_prod_size / out.num_sig_val;
        log::info("nvidia: fw: per-signature size: %u bytes (%u total / %u sigs)",
                  out.per_sig_size, out.hs_hdr.sig_prod_size, out.num_sig_val);
    }

    // Step 7: Set boot address
    // GA102: boot_addr = app[0].offset
    out.boot_addr = out.app.offset;
    log::info("nvidia: fw: boot address: 0x%x", out.boot_addr);

    // Set payload pointers
    out.payload = data + out.hdr.data_offset;
    out.payload_size = out.hdr.data_size;
    out.dma_valid = false;

    return OK;
}

// ============================================================================
// Bootloader Firmware Parsing
// ============================================================================

int32_t firmware_parse_bootloader(const uint8_t* data, uint32_t size, bootloader_fw& out) {
    // Step 1: Parse BinHdr
    int32_t rc = firmware_parse_binhdr(data, size, out.hdr);
    if (rc != OK) return rc;

    // Step 2: Parse RiscvUCodeDesc at header_offset
    uint32_t desc_off = out.hdr.header_offset;
    if (desc_off + sizeof(riscv_ucode_desc) > size) {
        log::error("nvidia: fw: RiscvUCodeDesc out of bounds at offset 0x%x", desc_off);
        return ERR_INVALID;
    }
    string::memcpy(&out.desc, data + desc_off, sizeof(riscv_ucode_desc));

    log::info("nvidia: fw: RiscvUCodeDesc: version=%u app_version=0x%08x",
              out.desc.version, out.desc.app_version);
    log::info("nvidia: fw:   bootloader: off=0x%x size=0x%x param_off=0x%x param_size=0x%x",
              out.desc.bootloader_offset, out.desc.bootloader_size,
              out.desc.bootloader_param_offset, out.desc.bootloader_param_size);
    log::info("nvidia: fw:   riscv_elf: off=0x%x size=0x%x",
              out.desc.riscv_elf_offset, out.desc.riscv_elf_size);
    log::info("nvidia: fw:   manifest: off=0x%x size=0x%x",
              out.desc.manifest_offset, out.desc.manifest_size);
    log::info("nvidia: fw:   monitor: code_off=0x%x code_size=0x%x data_off=0x%x data_size=0x%x",
              out.desc.monitor_code_offset, out.desc.monitor_code_size,
              out.desc.monitor_data_offset, out.desc.monitor_data_size);

    // Extract key offsets
    out.code_offset = out.desc.monitor_code_offset;
    out.data_offset = out.desc.monitor_data_offset;
    out.manifest_offset = out.desc.manifest_offset;
    out.app_version = out.desc.app_version;

    // Set payload pointers
    out.payload = data + out.hdr.data_offset;
    out.payload_size = out.hdr.data_size;
    out.dma_valid = false;

    return OK;
}

// ============================================================================
// GSP-RM Firmware ELF Parsing
// ============================================================================

// Helper: exact string comparison from a buffer against a target string
// (not prefix match — verifies the buffer string is null-terminated at the right point)
static bool str_eq(const uint8_t* buf, uint32_t buf_size, uint32_t offset,
                   const char* target) {
    uint32_t i = 0;
    for (; target[i] != '\0'; i++) {
        if (offset + i >= buf_size) return false;
        if (buf[offset + i] != static_cast<uint8_t>(target[i])) return false;
    }
    // Verify exact match: next char in buffer must be NUL (not a prefix match)
    if (offset + i < buf_size && buf[offset + i] != '\0') return false;
    return true;
}

int32_t firmware_parse_gsp_elf(const uint8_t* data, uint32_t size, gsp_fw& out) {
    out.parsed = false;
    out.fwimage = nullptr;
    out.fwimage_size = 0;
    out.fwsig = nullptr;
    out.fwsig_size = 0;

    // Step 1: Validate ELF magic
    if (size < sizeof(elf64_hdr)) {
        log::error("nvidia: fw: GSP firmware too small for ELF header (%u < %lu)",
                   size, sizeof(elf64_hdr));
        return ERR_INVALID;
    }

    uint32_t magic = rd32(data, 0);
    if (magic != ELF_MAGIC) {
        log::error("nvidia: fw: GSP firmware not ELF (magic=0x%08x, expected=0x%08x)",
                   magic, ELF_MAGIC);
        return ERR_INVALID;
    }

    // Step 2: Parse ELF64 header
    elf64_hdr ehdr;
    string::memcpy(&ehdr, data, sizeof(elf64_hdr));

    if (ehdr.e_ident_class != 2) { // ELFCLASS64
        log::error("nvidia: fw: GSP firmware not 64-bit ELF (class=%u)", ehdr.e_ident_class);
        return ERR_INVALID;
    }
    if (ehdr.e_ident_data != 1) { // ELFDATA2LSB
        log::error("nvidia: fw: GSP firmware not little-endian ELF");
        return ERR_INVALID;
    }

    log::info("nvidia: fw: ELF64: type=%u machine=%u shoff=0x%lx shnum=%u shstrndx=%u",
              ehdr.e_type, ehdr.e_machine, ehdr.e_shoff, ehdr.e_shnum, ehdr.e_shstrndx);

    // Step 3: Validate section header table
    if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0) {
        log::error("nvidia: fw: ELF has no section headers");
        return ERR_INVALID;
    }
    if (ehdr.e_shentsize != sizeof(elf64_shdr)) {
        log::error("nvidia: fw: unexpected shentsize %u (expected %lu)",
                   ehdr.e_shentsize, sizeof(elf64_shdr));
        return ERR_INVALID;
    }

    uint64_t sh_table_end = ehdr.e_shoff + static_cast<uint64_t>(ehdr.e_shnum) * ehdr.e_shentsize;
    if (sh_table_end > size) {
        log::error("nvidia: fw: section header table out of bounds");
        return ERR_INVALID;
    }

    // Step 4: Read the section header string table
    if (ehdr.e_shstrndx >= ehdr.e_shnum) {
        log::error("nvidia: fw: shstrndx out of range");
        return ERR_INVALID;
    }

    const elf64_shdr* shstrtab_hdr = reinterpret_cast<const elf64_shdr*>(
        data + ehdr.e_shoff + ehdr.e_shstrndx * sizeof(elf64_shdr));

    if (shstrtab_hdr->sh_offset + shstrtab_hdr->sh_size > size) {
        log::error("nvidia: fw: string table out of bounds");
        return ERR_INVALID;
    }

    const uint8_t* strtab = data + shstrtab_hdr->sh_offset;
    uint32_t strtab_size = static_cast<uint32_t>(shstrtab_hdr->sh_size);

    // Step 5: Iterate sections, find .fwimage and .fwsignature_ga10x
    for (uint16_t i = 0; i < ehdr.e_shnum; i++) {
        const elf64_shdr* shdr = reinterpret_cast<const elf64_shdr*>(
            data + ehdr.e_shoff + i * sizeof(elf64_shdr));

        if (shdr->sh_name >= strtab_size) continue;

        const char* name = reinterpret_cast<const char*>(strtab + shdr->sh_name);

        // Check for .fwimage
        if (str_eq(strtab, strtab_size, shdr->sh_name, ELF_SECTION_FWIMAGE)) {
            if (shdr->sh_offset + shdr->sh_size > size) {
                log::error("nvidia: fw: .fwimage section out of bounds");
                return ERR_INVALID;
            }
            out.fwimage = data + shdr->sh_offset;
            out.fwimage_size = static_cast<uint32_t>(shdr->sh_size);
            log::info("nvidia: fw: ELF section '%s': offset=0x%lx size=0x%lx (%u bytes)",
                      name, shdr->sh_offset, shdr->sh_size, out.fwimage_size);
        }

        // Check for .fwsignature_ga10x (Ampere)
        if (str_eq(strtab, strtab_size, shdr->sh_name, ELF_SECTION_FWSIG_GA10X)) {
            if (shdr->sh_offset + shdr->sh_size > size) {
                log::error("nvidia: fw: .fwsignature section out of bounds");
                return ERR_INVALID;
            }
            out.fwsig = data + shdr->sh_offset;
            out.fwsig_size = static_cast<uint32_t>(shdr->sh_size);
            log::info("nvidia: fw: ELF section '%s': offset=0x%lx size=0x%lx (%u bytes)",
                      name, shdr->sh_offset, shdr->sh_size, out.fwsig_size);
        }

        // Log all sections for debugging
        if (shdr->sh_size > 0 && shdr->sh_name < strtab_size) {
            log::debug("nvidia: fw: ELF section[%u] '%s': off=0x%lx size=0x%lx type=%u",
                       i, name, shdr->sh_offset, shdr->sh_size, shdr->sh_type);
        }
    }

    // Validate we found the required sections
    if (!out.fwimage || out.fwimage_size == 0) {
        log::error("nvidia: fw: .fwimage section not found in GSP ELF");
        return ERR_NOT_FOUND;
    }
    if (!out.fwsig || out.fwsig_size == 0) {
        log::warn("nvidia: fw: .fwsignature section not found (will try without)");
        // Non-fatal: the bootloader will verify signatures from the manifest
    }

    out.parsed = true;
    log::info("nvidia: fw: GSP ELF parsed: firmware=%u bytes, signatures=%u bytes",
              out.fwimage_size, out.fwsig_size);
    return OK;
}

// ============================================================================
// FWSEC Extraction from VBIOS
// ============================================================================

int32_t firmware_extract_fwsec(nv_gpu* gpu, fwsec_fw& out) {
    out.valid = false;

    const uint8_t* vbios = gpu->vbios_data();
    uint32_t vbios_size = gpu->vbios_size();

    if (!vbios || vbios_size == 0) {
        log::error("nvidia: fw: no VBIOS data available for FWSEC extraction");
        return ERR_NOT_FOUND;
    }

    log::info("nvidia: fw: extracting FWSEC from VBIOS (%u bytes)", vbios_size);

    // Step 1: Walk the ROM image chain to find the second FwSec (type 0xE0) image
    uint32_t offset = 0;
    uint32_t fwsec_image_offset = 0;
    uint32_t fwsec_image_size = 0;
    uint32_t pciat_image_size = 0;
    uint32_t first_fwsec_size = 0;
    uint32_t fwsec_count = 0;

    while (offset < vbios_size) {
        // Check ROM signature
        uint16_t sig = rd16(vbios, offset);
        if (sig != 0xAA55 && sig != 0xBB77 && sig != 0x4E56) {
            log::info("nvidia: fw: end of ROM chain at offset 0x%x (sig=0x%04x)", offset, sig);
            break;
        }

        // Find PCIR structure
        uint16_t pcir_off = rd16(vbios, offset + 0x18);
        uint32_t pcir_abs = offset + pcir_off;
        if (pcir_abs + 24 > vbios_size) break;

        // Validate PCIR signature
        uint32_t pcir_sig = rd32(vbios, pcir_abs);
        if (pcir_sig != 0x52494350) { // "PCIR"
            log::warn("nvidia: fw: expected PCIR at 0x%x, got 0x%08x", pcir_abs, pcir_sig);
            break;
        }

        uint16_t image_len_blocks = rd16(vbios, pcir_abs + 0x10);
        uint8_t code_type = vbios[pcir_abs + 0x14];
        uint8_t last_image = vbios[pcir_abs + 0x15];
        uint32_t image_size = image_len_blocks * 512;

        log::info("nvidia: fw: ROM image at 0x%x: type=0x%02x size=%u blocks=%u%s",
                  offset, code_type, image_size, image_len_blocks,
                  (last_image & 0x80) ? " [LAST]" : "");

        if (code_type == 0x00) { // PciAt
            pciat_image_size = image_size;
        } else if (code_type == 0xE0) { // FwSec
            fwsec_count++;
            if (fwsec_count == 1) {
                first_fwsec_size = image_size;
            } else if (fwsec_count == 2) {
                fwsec_image_offset = offset;
                fwsec_image_size = image_size;
                log::info("nvidia: fw: found second FwSec image at offset 0x%x (size=%u)",
                          fwsec_image_offset, fwsec_image_size);
            }
        }

        offset += image_size;
        if (last_image & 0x80) break;
    }

    if (fwsec_count < 2 || fwsec_image_size == 0) {
        log::error("nvidia: fw: could not find second FwSec image in VBIOS (found %u)", fwsec_count);
        return ERR_NOT_FOUND;
    }

    // Step 2: Find BIT token 0x70 (Falcon Data) in the PciAt image
    // We already have the BIT table from Phase 2 parsing
    uint32_t bit_offset = gpu->bit_offset();
    if (bit_offset == 0) {
        log::error("nvidia: fw: BIT table not available");
        return ERR_NOT_FOUND;
    }

    // BIT header: byte 8 = header_size, byte 9 = entry_size, byte 10 = entry_count
    uint8_t bit_hdr_size = vbios[bit_offset + 8];
    uint8_t entry_size = vbios[bit_offset + 9];
    uint8_t entry_count = vbios[bit_offset + 10];
    uint32_t falcon_data_ptr = 0;

    for (uint8_t i = 0; i < entry_count; i++) {
        uint32_t entry_off = bit_offset + bit_hdr_size + i * entry_size;
        if (entry_off + 6 > vbios_size) break;

        uint8_t token_id = vbios[entry_off];
        if (token_id == 0x70) { // Falcon Data token
            uint16_t data_off = rd16(vbios, entry_off + 4);
            uint16_t data_len = rd16(vbios, entry_off + 2);
            if (data_len >= 4 && data_off + 4 <= vbios_size) {
                falcon_data_ptr = rd32(vbios, data_off);
                log::info("nvidia: fw: BIT token 0x70 (Falcon): data_off=0x%04x → falcon_data_ptr=0x%08x",
                          data_off, falcon_data_ptr);
            }
            break;
        }
    }

    if (falcon_data_ptr == 0) {
        log::error("nvidia: fw: BIT token 0x70 (Falcon Data) not found");
        return ERR_NOT_FOUND;
    }

    // Step 3: Adjust falcon_data_ptr to be relative to the correct FwSec image.
    // The pointer assumes contiguous [PciAt][FwSec1][FwSec2] layout (ignoring EFI image).
    // Subtract PciAt size first, then check if the offset falls in FwSec1 or FwSec2.
    uint32_t adjusted = falcon_data_ptr - pciat_image_size;
    uint32_t pmu_table_abs;

    if (adjusted < first_fwsec_size) {
        // PMU table is in the first FwSec image (edge case on some GPUs)
        pmu_table_abs = (fwsec_image_offset - first_fwsec_size) + adjusted;
        log::info("nvidia: fw: PMU Lookup Table in FIRST FwSec: adjusted=0x%x abs=0x%x",
                  adjusted, pmu_table_abs);
    } else {
        // PMU table is in the second FwSec image (common case for GA102)
        uint32_t offset_in_fwsec2 = adjusted - first_fwsec_size;
        pmu_table_abs = fwsec_image_offset + offset_in_fwsec2;
        log::info("nvidia: fw: PMU Lookup Table in second FwSec: adjusted=0x%x offset=0x%x abs=0x%x",
                  adjusted, offset_in_fwsec2, pmu_table_abs);
    }

    log::info("nvidia: fw: PMU table adjustment: falcon_ptr=0x%x pciat=0x%x fwsec1=0x%x",
              falcon_data_ptr, pciat_image_size, first_fwsec_size);

    if (pmu_table_abs + 4 > vbios_size) {
        log::error("nvidia: fw: PMU Lookup Table out of bounds");
        return ERR_INVALID;
    }

    // Step 4: Parse PMU Lookup Table header
    uint8_t pmu_version = vbios[pmu_table_abs];
    uint8_t pmu_hdr_len = vbios[pmu_table_abs + 1];
    uint8_t pmu_entry_len = vbios[pmu_table_abs + 2];
    uint8_t pmu_entry_count = vbios[pmu_table_abs + 3];

    log::info("nvidia: fw: PMU Lookup: ver=%u hdr=%u entry_len=%u entries=%u",
              pmu_version, pmu_hdr_len, pmu_entry_len, pmu_entry_count);

    // Step 5: Find entry with app_id = 0x85 (FWSEC_PROD)
    uint32_t fwsec_desc_offset = 0;
    for (uint8_t i = 0; i < pmu_entry_count; i++) {
        uint32_t entry_abs = pmu_table_abs + pmu_hdr_len + i * pmu_entry_len;
        if (entry_abs + 6 > vbios_size) break;

        uint8_t app_id = vbios[entry_abs];
        uint8_t target_id = vbios[entry_abs + 1];
        uint32_t desc_ptr = rd32(vbios, entry_abs + 2);

        log::info("nvidia: fw: PMU entry[%u]: app_id=0x%02x target=0x%02x data=0x%08x",
                  i, app_id, target_id, desc_ptr);

        if (app_id == 0x85) { // FWSEC_PROD
            // Adjust offset same way as falcon_data_ptr
            uint32_t desc_adjusted = desc_ptr - pciat_image_size;
            if (desc_adjusted < first_fwsec_size) {
                log::error("nvidia: fw: FWSEC descriptor falls in first FwSec image — unsupported");
                return ERR_UNSUPPORTED;
            }
            fwsec_desc_offset = fwsec_image_offset + (desc_adjusted - first_fwsec_size);
            log::info("nvidia: fw: FWSEC_PROD descriptor: raw=0x%x adjusted=0x%x abs=0x%x",
                      desc_ptr, desc_adjusted, fwsec_desc_offset);
            break;
        }
    }

    if (fwsec_desc_offset == 0) {
        log::error("nvidia: fw: FWSEC_PROD (app_id=0x85) not found in PMU table");
        return ERR_NOT_FOUND;
    }

    // Step 6: Parse FalconUCodeDescV3
    if (fwsec_desc_offset + sizeof(falcon_ucode_desc_v3) > vbios_size) {
        log::error("nvidia: fw: FalconUCodeDescV3 out of bounds at 0x%x", fwsec_desc_offset);
        return ERR_INVALID;
    }

    string::memcpy(&out.desc, vbios + fwsec_desc_offset, sizeof(falcon_ucode_desc_v3));

    uint8_t desc_version = (out.desc.hdr >> 8) & 0xFF;
    uint16_t desc_hdr_size = (out.desc.hdr >> 16) & 0xFFFF;

    log::info("nvidia: fw: FalconUCodeDescV3: version=%u hdr_size=%u", desc_version, desc_hdr_size);
    log::info("nvidia: fw:   stored_size=%u pkc_data_off=0x%x interface_off=0x%x",
              out.desc.stored_size, out.desc.pkc_data_offset, out.desc.interface_offset);
    log::info("nvidia: fw:   imem: phys=0x%x load_size=0x%x virt=0x%x",
              out.desc.imem_phys_base, out.desc.imem_load_size, out.desc.imem_virt_base);
    log::info("nvidia: fw:   dmem: phys=0x%x load_size=0x%x",
              out.desc.dmem_phys_base, out.desc.dmem_load_size);
    log::info("nvidia: fw:   engine_id_mask=0x%04x ucode_id=%u sig_count=%u sig_versions=0x%04x",
              out.desc.engine_id_mask, out.desc.ucode_id,
              out.desc.signature_count, out.desc.signature_versions);

    if (desc_version != 3) {
        log::error("nvidia: fw: expected FalconUCodeDescV3 (version 3), got version %u", desc_version);
        return ERR_UNSUPPORTED;
    }

    // Validate hdr bit 0 (must be set per nouveau)
    if (!(out.desc.hdr & 0x00000001)) {
        log::error("nvidia: fw: FalconUCodeDescV3 hdr bit 0 not set (hdr=0x%08x)", out.desc.hdr);
        return ERR_INVALID;
    }

    // Step 7: Extract signatures
    // Layout: [FalconUCodeDescV3 (44 bytes)] [Signatures] [IMEM + DMEM ucode]
    // Signatures start at: desc_offset + sizeof(falcon_ucode_desc_v3) = desc_offset + 44
    // Ucode starts at: desc_offset + desc_hdr_size (hdr_size includes struct + signatures)
    out.sig_count = out.desc.signature_count;
    out.sig_size = RSA3K_SIG_SIZE;
    uint32_t sigs_offset = fwsec_desc_offset + static_cast<uint32_t>(sizeof(falcon_ucode_desc_v3));
    uint32_t sigs_total = out.sig_count * out.sig_size;

    log::info("nvidia: fw: FWSEC sigs: offset=0x%x (desc+%lu), count=%u, total=%u bytes",
              sigs_offset, sizeof(falcon_ucode_desc_v3), out.sig_count, sigs_total);

    if (sigs_offset + sigs_total > vbios_size) {
        log::error("nvidia: fw: FWSEC signatures out of bounds (0x%x + 0x%x > 0x%x)",
                   sigs_offset, sigs_total, vbios_size);
        return ERR_INVALID;
    }

    out.signatures = static_cast<uint8_t*>(heap::kzalloc(sigs_total));
    if (!out.signatures) {
        log::error("nvidia: fw: failed to allocate %u bytes for FWSEC signatures", sigs_total);
        return ERR_NOT_FOUND;
    }
    string::memcpy(out.signatures, vbios + sigs_offset, sigs_total);
    log::info("nvidia: fw: FWSEC: %u signatures extracted (%u bytes each)", out.sig_count, out.sig_size);

    // Step 8: Extract ucode (IMEM + DMEM)
    // Ucode starts at desc_offset + desc_hdr_size (NOT + sigs_total; hdr_size includes sigs)
    uint32_t ucode_offset = fwsec_desc_offset + desc_hdr_size;
    // Align DMEM load size to 256 bytes (falcon DMA requirement)
    uint32_t dmem_aligned = (out.desc.dmem_load_size + (DMEM_ALIGN - 1)) & ~(DMEM_ALIGN - 1);
    out.ucode_size = out.desc.imem_load_size + dmem_aligned;

    log::info("nvidia: fw: FWSEC ucode: offset=0x%x (desc+hdr_size=%u), "
              "imem=%u + dmem=%u (aligned %u) = %u bytes",
              ucode_offset, desc_hdr_size,
              out.desc.imem_load_size, out.desc.dmem_load_size, dmem_aligned, out.ucode_size);

    if (ucode_offset + out.ucode_size > vbios_size) {
        log::error("nvidia: fw: FWSEC ucode out of bounds (0x%x + 0x%x > 0x%x)",
                   ucode_offset, out.ucode_size, vbios_size);
        heap::kfree(out.signatures);
        out.signatures = nullptr;
        return ERR_INVALID;
    }

    out.ucode_data = static_cast<uint8_t*>(heap::kzalloc(out.ucode_size));
    if (!out.ucode_data) {
        log::error("nvidia: fw: failed to allocate %u bytes for FWSEC ucode", out.ucode_size);
        heap::kfree(out.signatures);
        out.signatures = nullptr;
        return ERR_NOT_FOUND;
    }
    string::memcpy(out.ucode_data, vbios + ucode_offset, out.ucode_size);

    // Store interface offset for later DMEM patching
    out.interface_offset = out.desc.interface_offset;
    out.dma_valid = false;
    out.valid = true;

    log::info("nvidia: fw: FWSEC extracted: ucode=%u bytes (IMEM=%u + DMEM=%u), %u sigs",
              out.ucode_size, out.desc.imem_load_size, out.desc.dmem_load_size, out.sig_count);

    return OK;
}

// ============================================================================
// Load All Firmware
// ============================================================================

int32_t firmware_load_all(nv_gpu* gpu, gsp_firmware& fw) {
    log::info("nvidia: ========================================");
    log::info("nvidia: Phase A: Loading GSP firmware");
    log::info("nvidia: ========================================");

    string::memset(&fw, 0, sizeof(fw));
    fw.all_loaded = false;
    int32_t rc;

    // 1. Load GSP-RM firmware (ELF, ~23MB)
    log::info("nvidia: fw: --- Loading GSP-RM firmware ---");
    rc = firmware_load_file(FW_PATH_GSP, fw.gsp.raw_data, fw.gsp.raw_size, FW_MAX_SIZE_GSP);
    if (rc != OK) {
        log::error("nvidia: fw: failed to load GSP-RM firmware");
        firmware_free_all(fw);
        return rc;
    }
    rc = firmware_parse_gsp_elf(fw.gsp.raw_data, fw.gsp.raw_size, fw.gsp);
    if (rc != OK) {
        log::error("nvidia: fw: failed to parse GSP-RM ELF");
        firmware_free_all(fw);
        return rc;
    }

    // 2. Load GSP bootloader (~4KB)
    log::info("nvidia: fw: --- Loading GSP bootloader ---");
    rc = firmware_load_file(FW_PATH_BOOTLOADER, fw.bootloader.raw_data,
                            fw.bootloader.raw_size, FW_MAX_SIZE_BOOTLOADER);
    if (rc != OK) {
        log::error("nvidia: fw: failed to load GSP bootloader");
        firmware_free_all(fw);
        return rc;
    }
    rc = firmware_parse_bootloader(fw.bootloader.raw_data, fw.bootloader.raw_size, fw.bootloader);
    if (rc != OK) {
        log::error("nvidia: fw: failed to parse GSP bootloader");
        firmware_free_all(fw);
        return rc;
    }

    // 3. Load booter_load (~57KB)
    log::info("nvidia: fw: --- Loading booter_load ---");
    rc = firmware_load_file(FW_PATH_BOOTER_LOAD, fw.booter_load.raw_data,
                            fw.booter_load.raw_size, FW_MAX_SIZE_BOOTER);
    if (rc != OK) {
        log::error("nvidia: fw: failed to load booter_load");
        firmware_free_all(fw);
        return rc;
    }
    rc = firmware_parse_booter(fw.booter_load.raw_data, fw.booter_load.raw_size, fw.booter_load);
    if (rc != OK) {
        log::error("nvidia: fw: failed to parse booter_load");
        firmware_free_all(fw);
        return rc;
    }

    // 4. Load booter_unload (~37KB)
    log::info("nvidia: fw: --- Loading booter_unload ---");
    rc = firmware_load_file(FW_PATH_BOOTER_UNLOAD, fw.booter_unload.raw_data,
                            fw.booter_unload.raw_size, FW_MAX_SIZE_BOOTER);
    if (rc != OK) {
        log::error("nvidia: fw: failed to load booter_unload");
        firmware_free_all(fw);
        return rc;
    }
    rc = firmware_parse_booter(fw.booter_unload.raw_data, fw.booter_unload.raw_size,
                               fw.booter_unload);
    if (rc != OK) {
        log::error("nvidia: fw: failed to parse booter_unload");
        firmware_free_all(fw);
        return rc;
    }

    // 5. Extract FWSEC from VBIOS
    log::info("nvidia: fw: --- Extracting FWSEC from VBIOS ---");
    rc = firmware_extract_fwsec(gpu, fw.fwsec);
    if (rc != OK) {
        log::error("nvidia: fw: FWSEC extraction failed");
        firmware_free_all(fw);
        return rc;
    }

    fw.all_loaded = true;

    log::info("nvidia: ========================================");
    log::info("nvidia: All firmware loaded and parsed:");
    log::info("nvidia:   GSP-RM:        %u bytes (fwimage=%u, sigs=%u)",
              fw.gsp.raw_size, fw.gsp.fwimage_size, fw.gsp.fwsig_size);
    log::info("nvidia:   Bootloader:    %u bytes (payload=%u, app_ver=0x%08x)",
              fw.bootloader.raw_size, fw.bootloader.payload_size, fw.bootloader.app_version);
    log::info("nvidia:   Booter load:   %u bytes (payload=%u)",
              fw.booter_load.raw_size, fw.booter_load.payload_size);
    log::info("nvidia:   Booter unload: %u bytes (payload=%u)",
              fw.booter_unload.raw_size, fw.booter_unload.payload_size);
    log::info("nvidia:   FWSEC:         %u bytes ucode, %u signatures",
              fw.fwsec.ucode_size, fw.fwsec.sig_count);
    log::info("nvidia: ========================================");

    return OK;
}

// ============================================================================
// Free All Firmware
// ============================================================================

static void free_booter(booter_fw& b) {
    if (b.raw_data) { heap::kfree(b.raw_data); b.raw_data = nullptr; }
    if (b.dma_valid) { dma::free_pages(b.dma); b.dma_valid = false; }
    b.payload = nullptr;
    b.payload_size = 0;
}

static void free_bootloader(bootloader_fw& b) {
    if (b.raw_data) { heap::kfree(b.raw_data); b.raw_data = nullptr; }
    if (b.dma_valid) { dma::free_pages(b.dma); b.dma_valid = false; }
    b.payload = nullptr;
    b.payload_size = 0;
}

static void free_gsp(gsp_fw& g) {
    if (g.raw_data) { heap::kfree(g.raw_data); g.raw_data = nullptr; }
    g.fwimage = nullptr;
    g.fwimage_size = 0;
    g.fwsig = nullptr;
    g.fwsig_size = 0;
    g.parsed = false;
}

static void free_fwsec(fwsec_fw& f) {
    if (f.ucode_data) { heap::kfree(f.ucode_data); f.ucode_data = nullptr; }
    if (f.signatures) { heap::kfree(f.signatures); f.signatures = nullptr; }
    if (f.dma_valid) { dma::free_pages(f.dma); f.dma_valid = false; }
    f.valid = false;
}

void firmware_free_all(gsp_firmware& fw) {
    free_booter(fw.booter_load);
    free_booter(fw.booter_unload);
    free_bootloader(fw.bootloader);
    free_gsp(fw.gsp);
    free_fwsec(fw.fwsec);
    fw.all_loaded = false;
}

} // namespace nv
