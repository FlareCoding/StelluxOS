#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_FIRMWARE_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_FIRMWARE_H

#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/nv_types.h"

namespace nv {

// Forward declaration
class nv_gpu;

// ============================================================================
// Firmware Binary Format Constants
// ============================================================================

// BinHdr magic (all non-ELF firmware files start with this)
constexpr uint32_t FW_BIN_MAGIC = 0x10DE;

// ELF magic (GSP firmware is an ELF64 binary)
constexpr uint32_t ELF_MAGIC = 0x464C457F; // "\x7FELF" little-endian

// GSP firmware ELF section names
constexpr const char* ELF_SECTION_FWIMAGE       = ".fwimage";
constexpr const char* ELF_SECTION_FWSIG_GA10X   = ".fwsignature_ga10x";
constexpr const char* ELF_SECTION_FWSIG_TU10X   = ".fwsignature_tu10x";

// RSA-3K signature size
constexpr uint32_t RSA3K_SIG_SIZE = 384;

// GSP page size (for radix-3 page tables)
constexpr uint32_t GSP_PAGE_SIZE  = 4096;
constexpr uint32_t GSP_PAGE_SHIFT = 12;

// Radix-3 page table: entries per page
constexpr uint32_t RADIX3_ENTRIES_PER_PAGE = GSP_PAGE_SIZE / 8; // 512

// DMEM alignment requirement
constexpr uint32_t DMEM_ALIGN = 256;

// Firmware version string for GA102 (r535.113.01)
constexpr const char* GSP_FW_VERSION = "535.113.01";

// Firmware file paths (relative to filesystem root)
constexpr const char* FW_PATH_GSP         = "/firmware/nvidia/ga102/gsp/gsp-535.113.01.bin";
constexpr const char* FW_PATH_BOOTLOADER  = "/firmware/nvidia/ga102/gsp/bootloader-535.113.01.bin";
constexpr const char* FW_PATH_BOOTER_LOAD = "/firmware/nvidia/ga102/gsp/booter_load-535.113.01.bin";
constexpr const char* FW_PATH_BOOTER_UNLOAD = "/firmware/nvidia/ga102/gsp/booter_unload-535.113.01.bin";

// Maximum firmware file sizes (sanity checks)
constexpr uint32_t FW_MAX_SIZE_GSP        = 32 * 1024 * 1024; // 32MB
constexpr uint32_t FW_MAX_SIZE_BOOTLOADER = 64 * 1024;        // 64KB
constexpr uint32_t FW_MAX_SIZE_BOOTER     = 128 * 1024;       // 128KB

// ============================================================================
// Firmware Binary Structures (on-disk format)
// ============================================================================

// Common firmware header (BinHdr) — used by bootloader, booter_load, booter_unload
struct __attribute__((packed)) bin_hdr {
    uint32_t bin_magic;       // Must be 0x10DE
    uint32_t bin_ver;         // Header format version
    uint32_t bin_size;        // Total binary size (may be ignored)
    uint32_t header_offset;   // Offset to application-specific header
    uint32_t data_offset;     // Offset to data payload
    uint32_t data_size;       // Size of data payload
};
static_assert(sizeof(bin_hdr) == 24);

// Heavy-Secured firmware header V2 (booter_load, booter_unload)
struct __attribute__((packed)) hs_header_v2 {
    uint32_t sig_prod_offset;    // Offset to production signatures
    uint32_t sig_prod_size;      // Total size of all production signatures
    uint32_t patch_loc;          // Offset to u32: DMEM offset for signature patch
    uint32_t patch_sig;          // Offset to u32: base index into signatures
    uint32_t meta_data_offset;   // Offset to signature metadata
    uint32_t meta_data_size;     // Size of signature metadata
    uint32_t num_sig;            // Offset to u32: number of signatures
    uint32_t header_offset;      // Offset to HsLoadHeaderV2
    uint32_t header_size;        // Size of HsLoadHeaderV2
};
static_assert(sizeof(hs_header_v2) == 36);

// HS signature parameters
struct __attribute__((packed)) hs_sig_params {
    uint32_t fuse_ver;           // Fuse version for signature selection
    uint32_t engine_id_mask;     // Falcon engine bitmask
    uint32_t ucode_id;           // Microcode ID for fuse lookup
};
static_assert(sizeof(hs_sig_params) == 12);

// Heavy-Secured load header V2
struct __attribute__((packed)) hs_load_header_v2 {
    uint32_t os_code_offset;     // Code (IMEM) start offset in payload
    uint32_t os_code_size;       // Total code section size
    uint32_t os_data_offset;     // Data (DMEM) start offset in payload
    uint32_t os_data_size;       // Data section size
    uint32_t num_apps;           // Number of app entries following
};
static_assert(sizeof(hs_load_header_v2) == 20);

// Per-app entry in HsLoadHeaderV2
struct __attribute__((packed)) hs_load_app_v2 {
    uint32_t offset;             // IMEM load offset
    uint32_t size;               // Code size
    uint32_t data_offset;        // DMEM load offset
    uint32_t data_size;          // Data size
};
static_assert(sizeof(hs_load_app_v2) == 16);

// RISC-V bootloader descriptor (GSP bootloader)
struct __attribute__((packed)) riscv_ucode_desc {
    uint32_t version;
    uint32_t bootloader_offset;
    uint32_t bootloader_size;
    uint32_t bootloader_param_offset;
    uint32_t bootloader_param_size;
    uint32_t riscv_elf_offset;
    uint32_t riscv_elf_size;
    uint32_t app_version;
    uint32_t manifest_offset;
    uint32_t manifest_size;
    uint32_t monitor_data_offset;
    uint32_t monitor_data_size;
    uint32_t monitor_code_offset;
    uint32_t monitor_code_size;
};
static_assert(sizeof(riscv_ucode_desc) == 56);

// ELF64 header (minimal — just what we need)
struct __attribute__((packed)) elf64_hdr {
    uint32_t e_ident_magic;      // 0x7F 'E' 'L' 'F'
    uint8_t  e_ident_class;      // 2 = 64-bit
    uint8_t  e_ident_data;       // 1 = little-endian
    uint8_t  e_ident_version;    // 1
    uint8_t  e_ident_osabi;
    uint8_t  e_ident_pad[8];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;            // Section header table offset
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;        // Size of each section header
    uint16_t e_shnum;            // Number of section headers
    uint16_t e_shstrndx;         // Section header string table index
};
static_assert(sizeof(elf64_hdr) == 64);

// ELF64 section header
struct __attribute__((packed)) elf64_shdr {
    uint32_t sh_name;            // Offset into string table
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;          // Offset in file
    uint64_t sh_size;            // Size of section
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};
static_assert(sizeof(elf64_shdr) == 64);

// Falcon ucode descriptor V3 (used by FWSEC from VBIOS)
struct __attribute__((packed)) falcon_ucode_desc_v3 {
    uint32_t hdr;                // bits[7:0]=unk, bits[15:8]=version(3), bits[31:16]=hdr_size
    uint32_t stored_size;
    uint32_t pkc_data_offset;    // DMEM offset for signature patch
    uint32_t interface_offset;   // Offset to app interface table in DMEM
    uint32_t imem_phys_base;
    uint32_t imem_load_size;
    uint32_t imem_virt_base;
    uint32_t dmem_phys_base;
    uint32_t dmem_load_size;
    uint16_t engine_id_mask;
    uint8_t  ucode_id;
    uint8_t  signature_count;
    uint16_t signature_versions; // Bitmask of included signature versions
    uint16_t reserved;
};
static_assert(sizeof(falcon_ucode_desc_v3) == 44);

// ============================================================================
// Parsed Firmware Data Structures (in-memory)
// ============================================================================

// Parsed booter firmware (for SEC2)
struct booter_fw {
    // Raw file data (heap-allocated, freed after DMA copy)
    uint8_t* raw_data;
    uint32_t raw_size;

    // Parsed headers
    bin_hdr          hdr;
    hs_header_v2     hs_hdr;
    hs_load_header_v2 load_hdr;
    hs_load_app_v2   app;        // First (and typically only) app entry

    // Dereferenced indirect values from HsHeaderV2
    uint32_t patch_loc_val;      // DMEM offset for signature patching
    uint32_t patch_sig_val;      // Base index into signatures array
    uint32_t num_sig_val;        // Number of available signatures

    // Signature metadata (for fuse-version-based signature selection)
    hs_sig_params sig_meta;

    // Boot address (falcon entry point)
    uint32_t boot_addr;

    // Per-signature size (sig_prod_size / num_sig_val)
    uint32_t per_sig_size;

    // Payload info
    const uint8_t* payload;      // Points into raw_data at hdr.data_offset
    uint32_t       payload_size; // = hdr.data_size

    // DMA buffer for the payload (used by SEC2 falcon)
    dma::buffer dma;
    bool        dma_valid;
};

// Parsed GSP bootloader firmware
struct bootloader_fw {
    // Raw file data
    uint8_t* raw_data;
    uint32_t raw_size;

    // Parsed headers
    bin_hdr          hdr;
    riscv_ucode_desc desc;

    // Payload info
    const uint8_t* payload;
    uint32_t       payload_size;

    // Key offsets (from descriptor)
    uint32_t code_offset;
    uint32_t data_offset;
    uint32_t manifest_offset;
    uint32_t app_version;

    // DMA buffer for the ucode payload
    dma::buffer dma;
    bool        dma_valid;
};

// Parsed GSP-RM firmware (ELF)
struct gsp_fw {
    // Raw file data (heap-allocated, ~23MB)
    uint8_t* raw_data;
    uint32_t raw_size;

    // Extracted from ELF
    const uint8_t* fwimage;      // Points into raw_data at .fwimage section
    uint32_t       fwimage_size;
    const uint8_t* fwsig;        // Points into raw_data at .fwsignature section
    uint32_t       fwsig_size;

    // DMA: GSP firmware uses radix-3 page tables, not flat DMA
    // These are allocated during GSP boot (Phase C), not here
    bool parsed;
};

// FWSEC firmware extracted from VBIOS
struct fwsec_fw {
    // The complete ucode (IMEM + DMEM concatenated)
    uint8_t* ucode_data;
    uint32_t ucode_size;

    // Parsed descriptor
    falcon_ucode_desc_v3 desc;

    // Signature info
    uint8_t* signatures;
    uint32_t sig_count;
    uint32_t sig_size;           // Per-signature size (384 for RSA-3K)

    // DMA buffer for the ucode
    dma::buffer dma;
    bool        dma_valid;

    // Interface info (for DMEM patching)
    uint32_t interface_offset;   // App interface table offset in DMEM
    uint32_t dmem_mapper_offset; // DMEM mapper offset in DMEM

    bool valid;
};

// Complete firmware collection for GSP boot
struct gsp_firmware {
    booter_fw     booter_load;
    booter_fw     booter_unload;
    bootloader_fw bootloader;
    gsp_fw        gsp;
    fwsec_fw      fwsec;

    bool all_loaded;
};

// ============================================================================
// Firmware Loading API
// ============================================================================

/**
 * Load all GSP firmware files from the filesystem into memory.
 * Parses headers and validates magic/format for each blob.
 * Does NOT allocate DMA or build page tables — that's done in Phase B/C.
 *
 * @param gpu  GPU device (for chip identification and VBIOS access)
 * @param fw   Output: populated firmware collection
 * @return OK on success, negative error code on failure
 */
int32_t firmware_load_all(nv_gpu* gpu, gsp_firmware& fw);

/**
 * Free all firmware memory (raw data, DMA buffers).
 */
void firmware_free_all(gsp_firmware& fw);

/**
 * Load a single firmware file from the filesystem.
 * Allocates memory via heap::uzalloc() and reads the file contents.
 *
 * @param path     Filesystem path (e.g., "/lib/firmware/nvidia/ga102/gsp/gsp-535.113.01.bin")
 * @param out_data Output: allocated buffer with file contents
 * @param out_size Output: file size in bytes
 * @param max_size Maximum allowed file size (sanity check)
 * @return OK on success, negative error code on failure
 */
int32_t firmware_load_file(const char* path, uint8_t*& out_data,
                           uint32_t& out_size, uint32_t max_size);

/**
 * Parse a BinHdr firmware blob (bootloader, booter_load, booter_unload).
 * Validates magic and extracts header/data offsets.
 */
int32_t firmware_parse_binhdr(const uint8_t* data, uint32_t size, bin_hdr& out);

/**
 * Parse a booter firmware blob (booter_load or booter_unload).
 * Extracts BinHdr + HsHeaderV2 + HsLoadHeaderV2 + app entries.
 */
int32_t firmware_parse_booter(const uint8_t* data, uint32_t size, booter_fw& out);

/**
 * Parse the GSP bootloader firmware blob.
 * Extracts BinHdr + RiscvUCodeDesc.
 */
int32_t firmware_parse_bootloader(const uint8_t* data, uint32_t size, bootloader_fw& out);

/**
 * Parse the GSP-RM firmware ELF64 binary.
 * Extracts .fwimage and .fwsignature sections.
 */
int32_t firmware_parse_gsp_elf(const uint8_t* data, uint32_t size, gsp_fw& out);

/**
 * Extract FWSEC firmware from the VBIOS ROM image.
 * Finds the second FwSec (type 0xE0) ROM image, locates the Falcon ucode
 * descriptor via BIT token 0x70 → PMU Lookup Table → app_id 0x85.
 */
int32_t firmware_extract_fwsec(nv_gpu* gpu, fwsec_fw& out);

} // namespace nv

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_FIRMWARE_H
