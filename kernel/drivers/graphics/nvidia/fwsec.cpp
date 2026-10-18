// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/fwsec.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/protected_memory.h"
#include "common/logging.h"
#include "common/string.h"

namespace drivers::nvidia {

// The versions FWSEC's signature versions can list, one bit each from version 0
constexpr uint32_t SIGNATURE_VERSION_COUNT = 8 * sizeof(fwsec_layout::signature_versions);

// The application interface's header and entry fields, as byte offsets from their starts. The entries follow the
// header, and NVIDIA's driver takes both at these sizes whatever the header's own size and version fields say.
constexpr uint32_t INTERFACE_ENTRY_COUNT       = 0x03; // 8 bits
constexpr uint32_t INTERFACE_HEADER_BYTES      = 4;
constexpr uint32_t INTERFACE_ENTRY_ID          = 0x00; // 32 bits
constexpr uint32_t INTERFACE_ENTRY_DATA_OFFSET = 0x04; // 32 bits, bytes from the start of FWSEC's data
constexpr uint32_t INTERFACE_ENTRY_BYTES       = 8;
constexpr uint32_t INTERFACE_MIN_ENTRIES       = 2; // as NVIDIA's driver requires, though only mapper entries are used
constexpr uint32_t DMEM_MAPPER_ID              = 0x4; // the id of an entry that leads to a DMEM mapper

// DMEM mapper fields, as byte offsets from its start, and its size as NVIDIA's driver takes it whatever its own
// size and version fields say
constexpr uint32_t MAPPER_INPUT_OFFSET = 0x08; // 32 bits, bytes from the start of FWSEC's data
constexpr uint32_t MAPPER_INPUT_SIZE   = 0x0C; // 32 bits, bytes
constexpr uint32_t MAPPER_INIT_COMMAND = 0x2C; // 32 bits, the id of the command FWSEC runs when it starts
constexpr uint32_t MAPPER_BYTES        = 64;

constexpr uint32_t FRTS_COMMAND_ID = 0x15;

// The FRTS command's 32-bit arguments as byte offsets: how FWSEC reads the VBIOS, then where the FRTS region lies.
// The 12 bytes from 0x08 are 0 as in NVIDIA's driver, and so are the 4 past the region's fields, which it leaves unset.
constexpr uint32_t READ_VBIOS_VERSION     = 0x00;
constexpr uint32_t READ_VBIOS_FIELDS_SIZE = 0x04; // bytes of the fields on reading the VBIOS
constexpr uint32_t READ_VBIOS_FLAGS       = 0x14;
constexpr uint32_t REGION_VERSION         = 0x18;
constexpr uint32_t REGION_FIELDS_SIZE     = 0x1C; // bytes of the region's fields
constexpr uint32_t REGION_OFFSET_4K       = 0x20; // in units of 4 KiB from the start of video memory
constexpr uint32_t REGION_SIZE_4K         = 0x24; // in units of 4 KiB
constexpr uint32_t REGION_MEDIA           = 0x28;
constexpr uint32_t FRTS_COMMAND_BYTES     = 48;

constexpr uint32_t ARGUMENTS_VERSION      = 1;
constexpr uint32_t READ_VBIOS_FIELD_BYTES = 24;
constexpr uint32_t REGION_FIELD_BYTES     = 20;
constexpr uint32_t REGION_IN_VIDEO_MEMORY = 2;
constexpr uint32_t UNIT_4K_SHIFT          = 12;

// The flags NVIDIA's driver always passes for reading the VBIOS, whose meaning its code does not give
constexpr uint32_t NVIDIA_READ_VBIOS_FLAGS = 2;

int32_t select_fwsec_signature(const registers& regs, const fwsec_layout& fwsec, fwsec_signature& signature_out) {
    uint32_t fuse_version = read_ucode_fuse_version(regs, falcon_id::gsp, fwsec.ucode_id);

    // Versions from 16 have no bit, and the bound keeps the shift below 32 bits, where it is defined
    bool has_version_bit = fuse_version < SIGNATURE_VERSION_COUNT;
    bool has_signature = has_version_bit && ((fwsec.signature_versions >> fuse_version) & 1) != 0;
    if (!has_signature) {
        log::error(
            "nvidia: FWSEC has no signature for fuse version %u, only for the versions whose bits are set in 0x%04X",
            fuse_version,
            fwsec.signature_versions
        );
        return ERR_BAD_VBIOS;
    }

    uint32_t index = 0;
    for (uint32_t version = 0; version < fuse_version; version++) {
        index += (fwsec.signature_versions >> version) & 1;
    }

    if (index >= fwsec.signature_count) {
        log::error(
            "nvidia: FWSEC's signature for fuse version %u comes after %u others, past its %u signatures",
            fuse_version,
            index,
            fwsec.signature_count
        );
        return ERR_BAD_VBIOS;
    }

    signature_out.fuse_version = static_cast<uint8_t>(fuse_version);
    signature_out.index = static_cast<uint8_t>(index);

    return OK;
}

// Finds the DMEM mapper through the application interface's last DMEM mapper entry. As in NVIDIA's driver, every
// entry up to the count must lie inside the data, and so must the mapper of every DMEM mapper entry.
static int32_t find_dmem_mapper(const uint8_t* data, const fwsec_layout& fwsec, uint32_t& mapper_out) {
    uint32_t interface = fwsec.interface_offset;
    if (!lies_inside(interface, INTERFACE_HEADER_BYTES, fwsec.data_size)) {
        log::error(
            "nvidia: FWSEC's %u-byte interface header at byte 0x%X of its data ends past its %u bytes of data",
            INTERFACE_HEADER_BYTES,
            interface,
            fwsec.data_size
        );
        return ERR_BAD_VBIOS;
    }

    uint8_t entry_count = data[interface + INTERFACE_ENTRY_COUNT];
    if (entry_count < INTERFACE_MIN_ENTRIES) {
        log::error(
            "nvidia: FWSEC's application interface lists %u entries, fewer than %u",
            entry_count,
            INTERFACE_MIN_ENTRIES
        );
        return ERR_BAD_VBIOS;
    }

    bool has_mapper = false;
    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t entry = static_cast<uint64_t>(interface) + INTERFACE_HEADER_BYTES + i * INTERFACE_ENTRY_BYTES;
        if (!lies_inside(entry, INTERFACE_ENTRY_BYTES, fwsec.data_size)) {
            log::error(
                "nvidia: entry %u of %u of FWSEC's interface at byte 0x%X of its data ends past its %u bytes of data",
                i + 1,
                entry_count,
                interface,
                fwsec.data_size
            );
            return ERR_BAD_VBIOS;
        }

        if (read_u32(data + entry + INTERFACE_ENTRY_ID) != DMEM_MAPPER_ID) {
            continue;
        }

        uint32_t mapper = read_u32(data + entry + INTERFACE_ENTRY_DATA_OFFSET);
        if (!lies_inside(mapper, MAPPER_BYTES, fwsec.data_size)) {
            log::error(
                "nvidia: FWSEC's %u-byte DMEM mapper at byte 0x%X of its data ends past its %u bytes of data",
                MAPPER_BYTES,
                mapper,
                fwsec.data_size
            );
            return ERR_BAD_VBIOS;
        }

        mapper_out = mapper;
        has_mapper = true;
    }

    if (!has_mapper) {
        log::error("nvidia: FWSEC's application interface has no DMEM mapper entry (id 0x%X)", DMEM_MAPPER_ID);
        return ERR_BAD_VBIOS;
    }

    return OK;
}

static void write_frts_command(uint8_t* input, uint64_t frts_offset) {
    uint32_t offset_4k = static_cast<uint32_t>(frts_offset >> UNIT_4K_SHIFT);
    uint32_t size_4k = static_cast<uint32_t>(FRTS_REGION_BYTES >> UNIT_4K_SHIFT);

    string::memset(input, 0, FRTS_COMMAND_BYTES);

    write_u32(input + READ_VBIOS_VERSION, ARGUMENTS_VERSION);
    write_u32(input + READ_VBIOS_FIELDS_SIZE, READ_VBIOS_FIELD_BYTES);
    write_u32(input + READ_VBIOS_FLAGS, NVIDIA_READ_VBIOS_FLAGS);

    write_u32(input + REGION_VERSION, ARGUMENTS_VERSION);
    write_u32(input + REGION_FIELDS_SIZE, REGION_FIELD_BYTES);
    write_u32(input + REGION_OFFSET_4K, offset_4k);
    write_u32(input + REGION_SIZE_4K, size_4k);
    write_u32(input + REGION_MEDIA, REGION_IN_VIDEO_MEMORY);
}

static int32_t patch_frts_command(uint8_t* data, const fwsec_layout& fwsec, uint64_t frts_offset) {
    uint32_t mapper = 0;
    int32_t rc = find_dmem_mapper(data, fwsec, mapper);
    if (rc != OK) {
        return rc;
    }

    uint32_t input_offset = read_u32(data + mapper + MAPPER_INPUT_OFFSET);
    uint32_t input_bytes = read_u32(data + mapper + MAPPER_INPUT_SIZE);
    if (input_bytes < FRTS_COMMAND_BYTES) {
        log::error(
            "nvidia: FWSEC's DMEM mapper gives an input buffer of %u bytes, smaller than the %u-byte FRTS command",
            input_bytes,
            FRTS_COMMAND_BYTES
        );
        return ERR_BAD_VBIOS;
    }

    if (!lies_inside(input_offset, FRTS_COMMAND_BYTES, fwsec.data_size)) {
        log::error(
            "nvidia: the %u-byte FRTS command at byte 0x%X of FWSEC's data ends past the data's %u bytes",
            FRTS_COMMAND_BYTES,
            input_offset,
            fwsec.data_size
        );
        return ERR_BAD_VBIOS;
    }

    write_frts_command(data + input_offset, frts_offset);
    write_u32(data + mapper + MAPPER_INIT_COMMAND, FRTS_COMMAND_ID);

    return OK;
}

int32_t prepare_fwsec_frts(
    const registers& rom,
    const fwsec_layout& fwsec,
    const fwsec_signature& signature,
    uint64_t frts_offset,
    uint8_t* image
) {
    uint8_t* data = image + fwsec.code_size;
    uint32_t signature_offset = fwsec.signatures_offset + signature.index * FWSEC_SIGNATURE_BYTES;

    copy_vbios_bytes(rom, fwsec.offset, fwsec.size, image);

    int32_t rc = patch_frts_command(data, fwsec, frts_offset);
    if (rc != OK) {
        return rc;
    }

    copy_vbios_bytes(rom, signature_offset, FWSEC_SIGNATURE_BYTES, data + fwsec.signature_slot);

    return OK;
}

falcon_program describe_fwsec_program(const fwsec_layout& fwsec, uint64_t dma_address, bool is_cached) {
    return {
        .dma_address = dma_address,
        .is_cached = is_cached,
        .reads_cached_memory = false,
        .code_offset = 0,
        .code_size = fwsec.code_size,
        .imem_physical_base = fwsec.imem_physical_base,
        .imem_virtual_base = fwsec.imem_virtual_base,
        .data_offset = fwsec.code_size,
        .data_size = fwsec.data_size,
        .dmem_physical_base = fwsec.dmem_physical_base,
        .signature_offset = fwsec.signature_slot,
        .engine_mask = fwsec.engine_mask,
        .ucode_id = fwsec.ucode_id,
    };
}

} // namespace drivers::nvidia
