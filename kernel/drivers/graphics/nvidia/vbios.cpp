// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/vbios.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"

namespace drivers::nvidia {

// Each structure opens with its signature, and NVIDIA's own images carry signatures of their own
constexpr uint16_t IMAGE_SIGNATURE           = 0xAA55;
constexpr uint16_t NVIDIA_IMAGE_SIGNATURE    = 0x4E56;
constexpr uint32_t PCI_DATA_SIGNATURE        = 0x52494350; // "PCIR"
constexpr uint32_t NVIDIA_PCI_DATA_SIGNATURE = 0x5344504E; // "NPDS"
constexpr uint32_t EXTENSION_SIGNATURE       = 0x4544504E; // "NPDE"

// Image header fields, as byte offsets from the start of the image
constexpr uint32_t HEADER_PCI_DATA_POINTER = 0x18; // 16 bits, from the start of the image

// PCI data structure fields, as byte offsets from its start
constexpr uint32_t PCI_DATA_LENGTH       = 0x0A; // 16 bits, bytes
constexpr uint32_t PCI_DATA_IMAGE_BLOCKS = 0x10; // 16 bits
constexpr uint32_t PCI_DATA_CODE_TYPE    = 0x14; // 8 bits
constexpr uint32_t PCI_DATA_INDICATOR    = 0x15; // 8 bits, bit 7 marks the last image

// NVIDIA's extension fields, as byte offsets from its start
constexpr uint32_t EXTENSION_LENGTH       = 0x06; // 16 bits, bytes
constexpr uint32_t EXTENSION_IMAGE_BLOCKS = 0x08; // 16 bits
constexpr uint32_t EXTENSION_FLAGS        = 0x0A; // 8 bits

// The extension starts at the first 16-byte boundary at or after the end of the PCI data structure
constexpr uint32_t EXTENSION_ALIGNMENT = 16;

// Bytes of each structure up to the end of the last field the walk reads
constexpr uint32_t HEADER_READ_BYTES    = HEADER_PCI_DATA_POINTER + 2;
constexpr uint32_t PCI_DATA_READ_BYTES  = PCI_DATA_INDICATOR + 1;
constexpr uint32_t EXTENSION_READ_BYTES = EXTENSION_FLAGS + 1;

// Bit 7 of the PCI data structure's indicator and of the extension's flags
constexpr uint8_t LAST_IMAGE = 0x80;

// Image sizes count blocks of 512 bytes
constexpr uint32_t IMAGE_BLOCK_BYTES = 512;

// The code types of x86 images and of NVIDIA's data. The first image of each places the base that pointers
// into NVIDIA's data count from.
constexpr uint8_t X86_CODE    = 0x00;
constexpr uint8_t NVIDIA_DATA = 0xE0;

// The BIT, NVIDIA's index of the VBIOS, opens with this id and signature
constexpr uint16_t BIT_ID        = 0xB8FF;
constexpr uint32_t BIT_SIGNATURE = 0x00544942; // "BIT\0"

// BIT header fields, as byte offsets from its start. Its tokens follow the header.
constexpr uint32_t BIT_SIGNATURE_FIELD    = 0x02; // 32 bits
constexpr uint32_t BIT_HEADER_SIZE        = 0x08; // 8 bits, bytes, all of which the checksum covers
constexpr uint32_t BIT_TOKEN_SIZE         = 0x09; // 8 bits, bytes
constexpr uint32_t BIT_TOKEN_COUNT        = 0x0A; // 8 bits
constexpr uint32_t BIT_HEADER_FIELD_BYTES = 12;

// BIT token fields, as byte offsets from its start
constexpr uint32_t TOKEN_ID           = 0x00; // 8 bits
constexpr uint32_t TOKEN_DATA_VERSION = 0x01; // 8 bits
constexpr uint32_t TOKEN_DATA_SIZE    = 0x02; // 16 bits, bytes
constexpr uint32_t TOKEN_DATA_OFFSET  = 0x04; // 16 bits, bytes from the start of the VBIOS
constexpr uint32_t TOKEN_FIELD_BYTES  = 6;

// Version 2 of the falcon data token's data holds a 32-bit pointer into NVIDIA's data to the falcon program table
constexpr uint8_t  FALCON_DATA_TOKEN       = 0x70;
constexpr uint8_t  FALCON_DATA_VERSION     = 2;
constexpr uint32_t FALCON_DATA_FIELD_BYTES = 4;

// Falcon program table header fields, as byte offsets from its start. Its entries follow the header.
constexpr uint32_t TABLE_VERSION            = 0x00; // 8 bits
constexpr uint32_t TABLE_HEADER_SIZE        = 0x01; // 8 bits, bytes
constexpr uint32_t TABLE_ENTRY_SIZE         = 0x02; // 8 bits, bytes
constexpr uint32_t TABLE_ENTRY_COUNT        = 0x03; // 8 bits
constexpr uint8_t  TABLE_VERSION_1          = 1;
constexpr uint32_t TABLE_HEADER_FIELD_BYTES = 6;
constexpr uint32_t TABLE_ENTRY_FIELD_BYTES  = 6;

// Falcon program table entry fields, as byte offsets from its start
constexpr uint32_t ENTRY_APPLICATION = 0x00; // 8 bits, the app id saying which program the entry is for
constexpr uint32_t ENTRY_DESCRIPTOR  = 0x02; // 32 bits, a pointer into NVIDIA's data to the program's descriptor

// The app id of production FWSEC's entry
constexpr uint8_t FWSEC_PRODUCTION = 0x85;

// The descriptor's first 32 bits: bit 0 is set when bits 15:8 give its version, and bits 31:16 give its size in
// bytes, which covers its fields and the signatures after them
constexpr uint32_t DESCRIPTOR_HAS_VERSION   = 0x1;
constexpr uint32_t DESCRIPTOR_VERSION_SHIFT = 8;
constexpr uint32_t DESCRIPTOR_SIZE_SHIFT    = 16;
constexpr uint8_t  DESCRIPTOR_VERSION_3     = 3;

// Version 3 descriptor fields, as byte offsets from its start
constexpr uint32_t DESCRIPTOR_STORED_SIZE        = 0x04; // 32 bits, bytes of the program as stored, before rounding
constexpr uint32_t DESCRIPTOR_SIGNATURE_SLOT     = 0x08; // 32 bits, bytes from the start of the program's data
constexpr uint32_t DESCRIPTOR_INTERFACE_OFFSET   = 0x0C; // 32 bits, bytes from the start of the program's data
constexpr uint32_t DESCRIPTOR_CODE_SIZE          = 0x14; // 32 bits, bytes
constexpr uint32_t DESCRIPTOR_DATA_SIZE          = 0x20; // 32 bits, bytes
constexpr uint32_t DESCRIPTOR_UCODE_ID           = 0x26; // 8 bits
constexpr uint32_t DESCRIPTOR_SIGNATURE_COUNT    = 0x27; // 8 bits
constexpr uint32_t DESCRIPTOR_SIGNATURE_VERSIONS = 0x28; // 16 bits
constexpr uint32_t DESCRIPTOR_FIELD_BYTES        = 44;

// FWSEC spans its stored size rounded up to a multiple of 256 bytes, the bytes copied from the VBIOS to load it
constexpr uint64_t FWSEC_SIZE_ALIGNMENT = 256;

// A `registers` view reads only aligned 32-bit words, so fields are built a byte at a time from the words holding them
static uint8_t read_rom_u8(const registers& rom, uint32_t offset) {
    uint32_t word = rom.read(offset & ~3u);
    uint32_t shift = (offset & 3) * 8;
    return static_cast<uint8_t>(word >> shift);
}

static uint16_t read_rom_u16(const registers& rom, uint32_t offset) {
    uint32_t low = read_rom_u8(rom, offset);
    uint32_t high = read_rom_u8(rom, offset + 1);
    return static_cast<uint16_t>(low | (high << 8));
}

static uint32_t read_rom_u32(const registers& rom, uint32_t offset) {
    uint32_t low = read_rom_u16(rom, offset);
    uint32_t high = read_rom_u16(rom, offset + 2);
    return low | (high << 16);
}

static bool is_inside_rom_window(uint32_t offset, uint32_t bytes) {
    return lies_inside(offset, bytes, ROM_WINDOW_BYTES);
}

// The VBIOS ends with its last image, so every structure read from it must end at or before that image's end
static uint32_t end_of_vbios(const vbios_layout& layout) {
    const vbios_image& last_image = layout.images[layout.image_count - 1];
    return last_image.offset + last_image.size;
}

// Reads the size and code type of the image at `offset` from the structures that
// describe it, and whether the chain ends with it
static int32_t read_image_layout(
    const registers& rom,
    uint32_t offset,
    vbios_image& image_out,
    bool& is_last_out
) {
    bool has_header = is_inside_rom_window(offset, HEADER_READ_BYTES);
    uint16_t signature = has_header ? read_rom_u16(rom, offset) : 0;
    if (signature != IMAGE_SIGNATURE && signature != NVIDIA_IMAGE_SIGNATURE) {
        log::error("nvidia: no VBIOS image signature at 0x%X", offset);
        return ERR_BAD_VBIOS;
    }

    uint32_t pci_data_offset = offset + read_rom_u16(rom, offset + HEADER_PCI_DATA_POINTER);
    bool has_pci_data = is_inside_rom_window(pci_data_offset, PCI_DATA_READ_BYTES);
    uint32_t pci_data_signature = has_pci_data ? read_rom_u32(rom, pci_data_offset) : 0;
    if (pci_data_signature != PCI_DATA_SIGNATURE && pci_data_signature != NVIDIA_PCI_DATA_SIGNATURE) {
        log::error("nvidia: the VBIOS image at 0x%X has no PCI data structure", offset);
        return ERR_BAD_VBIOS;
    }

    uint32_t blocks = read_rom_u16(rom, pci_data_offset + PCI_DATA_IMAGE_BLOCKS);
    uint8_t code_type = read_rom_u8(rom, pci_data_offset + PCI_DATA_CODE_TYPE);
    bool is_last = (read_rom_u8(rom, pci_data_offset + PCI_DATA_INDICATOR) & LAST_IMAGE) != 0;

    uint32_t pci_data_end = pci_data_offset + read_rom_u16(rom, pci_data_offset + PCI_DATA_LENGTH);
    uint32_t extension_offset = (pci_data_end + EXTENSION_ALIGNMENT - 1) & ~(EXTENSION_ALIGNMENT - 1);
    bool has_extension = is_inside_rom_window(extension_offset, EXTENSION_READ_BYTES)
        && read_rom_u32(rom, extension_offset) == EXTENSION_SIGNATURE;
    if (has_extension) {
        if (read_rom_u16(rom, extension_offset + EXTENSION_LENGTH) < EXTENSION_READ_BYTES) {
            log::error("nvidia: the VBIOS image at 0x%X has an extension too short for its flags", offset);
            return ERR_BAD_VBIOS;
        }

        blocks = read_rom_u16(rom, extension_offset + EXTENSION_IMAGE_BLOCKS);
        is_last = (read_rom_u8(rom, extension_offset + EXTENSION_FLAGS) & LAST_IMAGE) != 0;
    }

    uint32_t size = blocks * IMAGE_BLOCK_BYTES;
    if (size == 0) {
        log::error("nvidia: the VBIOS image at 0x%X has a size of zero", offset);
        return ERR_BAD_VBIOS;
    }

    if (!is_inside_rom_window(offset, size)) {
        log::error("nvidia: the VBIOS image at 0x%X is %u bytes long and ends past the ROM window", offset, size);
        return ERR_BAD_VBIOS;
    }

    image_out.offset = offset;
    image_out.size = size;
    image_out.code_type = code_type;
    is_last_out = is_last;

    return OK;
}

int32_t read_vbios_layout(const registers& rom, vbios_layout& layout_out) {
    layout_out.image_count = 0;

    uint32_t offset = 0;
    bool is_last = false;
    while (!is_last) {
        if (layout_out.image_count == VBIOS_MAX_IMAGES) {
            log::error("nvidia: the VBIOS chain does not end within %zu images", VBIOS_MAX_IMAGES);
            return ERR_BAD_VBIOS;
        }

        vbios_image& image = layout_out.images[layout_out.image_count];
        int32_t rc = read_image_layout(rom, offset, image, is_last);
        if (rc != OK) {
            return rc;
        }

        layout_out.image_count++;
        offset += image.size;
    }

    return OK;
}

static int32_t find_nvidia_data_base(const vbios_layout& layout, uint32_t& base_out) {
    const vbios_image* x86_image = nullptr;
    const vbios_image* nvidia_image = nullptr;
    for (size_t i = 0; i < layout.image_count; i++) {
        const vbios_image& image = layout.images[i];
        if (!x86_image && image.code_type == X86_CODE) {
            x86_image = &image;
        }

        if (!nvidia_image && image.code_type == NVIDIA_DATA) {
            nvidia_image = &image;
        }
    }

    if (!x86_image || !nvidia_image) {
        log::error(
            "nvidia: the VBIOS lacks an x86 image (code type 0x%02X) or an image of NVIDIA's data (code type 0x%02X)",
            X86_CODE,
            NVIDIA_DATA
        );
        return ERR_BAD_VBIOS;
    }

    bool has_nvidia_data_base = nvidia_image->offset >= x86_image->size;
    if (!has_nvidia_data_base) {
        log::error(
            "nvidia: the first image of NVIDIA's data starts at 0x%X, below the first x86 image's size of 0x%X bytes",
            nvidia_image->offset,
            x86_image->size
        );
        return ERR_BAD_VBIOS;
    }

    base_out = nvidia_image->offset - x86_image->size;

    return OK;
}

// Scans from the start of the VBIOS, since nothing points to the BIT
static bool find_bit(const registers& rom, uint32_t vbios_end, uint32_t& bit_out) {
    for (uint32_t offset = 0; lies_inside(offset, BIT_HEADER_FIELD_BYTES, vbios_end); offset++) {
        bool has_id = read_rom_u16(rom, offset) == BIT_ID;
        bool has_id_and_signature = has_id && read_rom_u32(rom, offset + BIT_SIGNATURE_FIELD) == BIT_SIGNATURE;
        if (!has_id_and_signature) {
            continue;
        }

        uint32_t header_size = read_rom_u8(rom, offset + BIT_HEADER_SIZE);
        bool covers_fields = header_size >= BIT_HEADER_FIELD_BYTES;
        if (!covers_fields || !lies_inside(offset, header_size, vbios_end)) {
            continue;
        }

        uint8_t sum = 0;
        for (uint32_t i = 0; i < header_size; i++) {
            sum = static_cast<uint8_t>(sum + read_rom_u8(rom, offset + i));
        }

        if (sum == 0) {
            bit_out = offset;
            return true;
        }
    }

    return false;
}

// Reads the table's pointer from the data of the BIT's first falcon data token of version 2 with at least
// `FALCON_DATA_FIELD_BYTES` bytes of data, refusing short tokens and any read that ends past the VBIOS
static int32_t read_table_pointer(const registers& rom, uint32_t bit, uint32_t vbios_end, uint32_t& pointer_out) {
    uint32_t header_size = read_rom_u8(rom, bit + BIT_HEADER_SIZE);
    uint32_t token_size = read_rom_u8(rom, bit + BIT_TOKEN_SIZE);
    uint32_t token_count = read_rom_u8(rom, bit + BIT_TOKEN_COUNT);
    if (token_size < TOKEN_FIELD_BYTES) {
        log::error(
            "nvidia: the BIT at 0x%X has %u-byte tokens, shorter than their %u bytes of fields",
            bit,
            token_size,
            TOKEN_FIELD_BYTES
        );
        return ERR_BAD_VBIOS;
    }

    for (uint32_t i = 0; i < token_count; i++) {
        uint32_t token = bit + header_size + i * token_size;
        if (!lies_inside(token, TOKEN_FIELD_BYTES, vbios_end)) {
            log::error("nvidia: token %u of the BIT at 0x%X ends past the VBIOS", i, bit);
            return ERR_BAD_VBIOS;
        }

        bool is_falcon_data_token = read_rom_u8(rom, token + TOKEN_ID) == FALCON_DATA_TOKEN;
        bool is_version_2 = read_rom_u8(rom, token + TOKEN_DATA_VERSION) == FALCON_DATA_VERSION;
        bool holds_pointer = read_rom_u16(rom, token + TOKEN_DATA_SIZE) >= FALCON_DATA_FIELD_BYTES;
        if (!is_falcon_data_token || !is_version_2 || !holds_pointer) {
            continue;
        }

        uint32_t falcon_data = read_rom_u16(rom, token + TOKEN_DATA_OFFSET);
        if (!lies_inside(falcon_data, FALCON_DATA_FIELD_BYTES, vbios_end)) {
            log::error("nvidia: the BIT's falcon data at 0x%X ends past the VBIOS", falcon_data);
            return ERR_BAD_VBIOS;
        }

        pointer_out = read_rom_u32(rom, falcon_data);
        return OK;
    }

    log::error(
        "nvidia: the BIT at 0x%X has no falcon data token of version %u with at least %u bytes of data",
        bit,
        FALCON_DATA_VERSION,
        FALCON_DATA_FIELD_BYTES
    );
    return ERR_BAD_VBIOS;
}

// Checks the version and sizes in the table's header, whose fields must lie inside the VBIOS, and that every
// entry lies inside it as well
static int32_t read_table_header(
    const registers& rom,
    uint32_t table,
    uint32_t vbios_end,
    falcon_program_table& table_out
) {
    uint8_t version = read_rom_u8(rom, table + TABLE_VERSION);
    uint8_t header_size = read_rom_u8(rom, table + TABLE_HEADER_SIZE);
    uint8_t entry_size = read_rom_u8(rom, table + TABLE_ENTRY_SIZE);
    uint8_t entry_count = read_rom_u8(rom, table + TABLE_ENTRY_COUNT);

    bool is_version_1 = version == TABLE_VERSION_1;
    bool holds_fields = header_size >= TABLE_HEADER_FIELD_BYTES && entry_size >= TABLE_ENTRY_FIELD_BYTES;
    if (!is_version_1 || !holds_fields) {
        log::error(
            "nvidia: the falcon program table at 0x%X is version %u with a %u-byte header and %u-byte entries, "
            "instead of version %u with a header of at least %u bytes and entries of at least %u",
            table,
            version,
            header_size,
            entry_size,
            TABLE_VERSION_1,
            TABLE_HEADER_FIELD_BYTES,
            TABLE_ENTRY_FIELD_BYTES
        );
        return ERR_BAD_VBIOS;
    }

    uint32_t table_bytes = header_size + static_cast<uint32_t>(entry_count) * entry_size;
    if (!lies_inside(table, table_bytes, vbios_end)) {
        log::error(
            "nvidia: the falcon program table at 0x%X ends past the VBIOS with its %u-byte header and %u entries "
            "of %u bytes",
            table,
            header_size,
            entry_count,
            entry_size
        );
        return ERR_BAD_VBIOS;
    }

    table_out.offset = table;
    table_out.header_size = header_size;
    table_out.entry_size = entry_size;
    table_out.entry_count = entry_count;

    return OK;
}

int32_t find_falcon_program_table(const registers& rom, const vbios_layout& layout, falcon_program_table& table_out) {
    uint32_t base = 0;
    int32_t rc = find_nvidia_data_base(layout, base);
    if (rc != OK) {
        return rc;
    }

    uint32_t vbios_end = end_of_vbios(layout);

    uint32_t bit = 0;
    if (!find_bit(rom, vbios_end, bit)) {
        log::error("nvidia: the VBIOS has no BIT");
        return ERR_BAD_VBIOS;
    }

    uint32_t pointer = 0;
    rc = read_table_pointer(rom, bit, vbios_end, pointer);
    if (rc != OK) {
        return rc;
    }

    if (!lies_inside(pointer, TABLE_HEADER_FIELD_BYTES, vbios_end - base)) {
        log::error(
            "nvidia: the falcon program table's header, at the base 0x%X plus its pointer 0x%X, ends past the VBIOS",
            base,
            pointer
        );
        return ERR_BAD_VBIOS;
    }

    rc = read_table_header(rom, base + pointer, vbios_end, table_out);
    if (rc != OK) {
        return rc;
    }

    table_out.base = base;

    return OK;
}

// Finds the table's first production FWSEC entry. The table lies inside the VBIOS with all its entries, so their
// fields need no bound check.
static bool find_fwsec_entry(const registers& rom, const falcon_program_table& table, uint32_t& entry_out) {
    for (uint32_t i = 0; i < table.entry_count; i++) {
        uint32_t entry = table.offset + table.header_size + i * table.entry_size;
        if (read_rom_u8(rom, entry + ENTRY_APPLICATION) == FWSEC_PRODUCTION) {
            entry_out = entry;
            return true;
        }
    }

    return false;
}

// Checks that the descriptor, whose fields must lie inside the VBIOS, is version 3 and lists and holds its signatures,
// then reads where FWSEC lies, checking that it lies inside the VBIOS, its code and data in it, its slot in its data
static int32_t read_fwsec_descriptor(
    const registers& rom,
    uint32_t descriptor,
    uint32_t vbios_end,
    fwsec_layout& fwsec_out
) {
    uint32_t header = read_rom_u32(rom, descriptor);
    bool has_version = (header & DESCRIPTOR_HAS_VERSION) != 0;
    if (!has_version) {
        log::error("nvidia: FWSEC's descriptor at 0x%X gives no version", descriptor);
        return ERR_BAD_VBIOS;
    }

    uint8_t version = static_cast<uint8_t>(header >> DESCRIPTOR_VERSION_SHIFT);
    if (version != DESCRIPTOR_VERSION_3) {
        log::error(
            "nvidia: FWSEC's descriptor at 0x%X is version %u instead of %u",
            descriptor,
            version,
            DESCRIPTOR_VERSION_3
        );
        return ERR_BAD_VBIOS;
    }

    uint8_t signature_count = read_rom_u8(rom, descriptor + DESCRIPTOR_SIGNATURE_COUNT);
    if (signature_count == 0) {
        log::error("nvidia: FWSEC's descriptor at 0x%X lists no signatures", descriptor);
        return ERR_BAD_VBIOS;
    }

    uint32_t descriptor_size = header >> DESCRIPTOR_SIZE_SHIFT;
    uint32_t fields_and_signatures = DESCRIPTOR_FIELD_BYTES + signature_count * FWSEC_SIGNATURE_BYTES;
    if (descriptor_size < fields_and_signatures) {
        log::error(
            "nvidia: FWSEC's descriptor at 0x%X is %u bytes, fewer than its %u bytes of fields and %u signatures of "
            "%u bytes",
            descriptor,
            descriptor_size,
            DESCRIPTOR_FIELD_BYTES,
            signature_count,
            FWSEC_SIGNATURE_BYTES
        );
        return ERR_BAD_VBIOS;
    }

    uint32_t fwsec_offset = descriptor + descriptor_size;
    uint64_t stored_size = read_rom_u32(rom, descriptor + DESCRIPTOR_STORED_SIZE);
    uint64_t fwsec_size = (stored_size + FWSEC_SIZE_ALIGNMENT - 1) / FWSEC_SIZE_ALIGNMENT * FWSEC_SIZE_ALIGNMENT;
    if (!lies_inside(fwsec_offset, fwsec_size, vbios_end)) {
        log::error("nvidia: FWSEC at 0x%X ends past the VBIOS with its %lu bytes", fwsec_offset, fwsec_size);
        return ERR_BAD_VBIOS;
    }

    uint32_t code_size = read_rom_u32(rom, descriptor + DESCRIPTOR_CODE_SIZE);
    uint32_t data_size = read_rom_u32(rom, descriptor + DESCRIPTOR_DATA_SIZE);
    if (!lies_inside(code_size, data_size, fwsec_size)) {
        log::error(
            "nvidia: FWSEC's %u bytes of code and %u bytes of data do not fit in its %lu bytes",
            code_size,
            data_size,
            fwsec_size
        );
        return ERR_BAD_VBIOS;
    }

    uint32_t signature_slot = read_rom_u32(rom, descriptor + DESCRIPTOR_SIGNATURE_SLOT);
    if (!lies_inside(signature_slot, FWSEC_SIGNATURE_BYTES, data_size)) {
        log::error(
            "nvidia: FWSEC's %u-byte signature slot at byte 0x%X of its data ends past its %u bytes of data",
            FWSEC_SIGNATURE_BYTES,
            signature_slot,
            data_size
        );
        return ERR_BAD_VBIOS;
    }

    fwsec_out.offset = fwsec_offset;
    fwsec_out.size = static_cast<uint32_t>(fwsec_size);
    fwsec_out.code_size = code_size;
    fwsec_out.data_size = data_size;
    fwsec_out.signature_slot = signature_slot;
    fwsec_out.interface_offset = read_rom_u32(rom, descriptor + DESCRIPTOR_INTERFACE_OFFSET);
    fwsec_out.signatures_offset = descriptor + DESCRIPTOR_FIELD_BYTES;
    fwsec_out.signature_count = signature_count;
    fwsec_out.ucode_id = read_rom_u8(rom, descriptor + DESCRIPTOR_UCODE_ID);
    fwsec_out.signature_versions = read_rom_u16(rom, descriptor + DESCRIPTOR_SIGNATURE_VERSIONS);

    return OK;
}

int32_t find_fwsec(
    const registers& rom,
    const vbios_layout& layout,
    const falcon_program_table& table,
    fwsec_layout& fwsec_out
) {
    uint32_t vbios_end = end_of_vbios(layout);

    uint32_t entry = 0;
    if (!find_fwsec_entry(rom, table, entry)) {
        log::error(
            "nvidia: the falcon program table at 0x%X has no production FWSEC entry (app id 0x%02X)",
            table.offset,
            FWSEC_PRODUCTION
        );
        return ERR_BAD_VBIOS;
    }

    uint32_t pointer = read_rom_u32(rom, entry + ENTRY_DESCRIPTOR);
    if (!lies_inside(pointer, DESCRIPTOR_FIELD_BYTES, vbios_end - table.base)) {
        log::error(
            "nvidia: the %u bytes of fields of FWSEC's descriptor, at the base 0x%X plus its pointer 0x%X, end past "
            "the VBIOS",
            DESCRIPTOR_FIELD_BYTES,
            table.base,
            pointer
        );
        return ERR_BAD_VBIOS;
    }

    return read_fwsec_descriptor(rom, table.base + pointer, vbios_end, fwsec_out);
}

void copy_vbios_bytes(const registers& rom, uint32_t offset, uint32_t bytes, uint8_t* out) {
    uint32_t word = 0;

    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t position = offset + i;
        bool starts_word = i == 0 || (position & 3) == 0;
        if (starts_word) {
            word = rom.read(position & ~3u);
        }

        out[i] = static_cast<uint8_t>(word >> ((position & 3) * 8));
    }
}

} // namespace drivers::nvidia
