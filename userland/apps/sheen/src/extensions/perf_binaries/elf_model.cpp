#include "extensions/perf_binaries/elf_model.h"

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define SHEEN_HAS_CXA_DEMANGLE 1
#else
#define SHEEN_HAS_CXA_DEMANGLE 0
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>

namespace sheen {

static constexpr uint8_t ELF_MAGIC_BYTES[4] = { 0x7f, 'E', 'L', 'F' };
static constexpr size_t EI_CLASS_OFFSET = 4, EI_DATA_OFFSET = 5, EI_VERSION_OFFSET = 6, MIN_TYPED_PREFIX_BYTES = 20;
static constexpr uint8_t ELF_CLASS_64 = 2, ELF_DATA_LSB = 1, ELF_CURRENT_VERSION = 1;
static constexpr size_t ELF64_HEADER_SIZE = 64, ELF64_PHDR_SIZE = 56, ELF64_SHDR_SIZE = 64;
static constexpr size_t ELF64_SYM_SIZE = 24, ELF64_SHNDX_ENTRY_SIZE = 4;

static constexpr size_t EHDR_TYPE_OFFSET = 0x10, EHDR_MACHINE_OFFSET = 0x12, EHDR_VERSION_OFFSET = 0x14;
static constexpr size_t EHDR_ENTRY_OFFSET = 0x18, EHDR_PHOFF_OFFSET = 0x20, EHDR_SHOFF_OFFSET = 0x28;
static constexpr size_t EHDR_FLAGS_OFFSET = 0x30, EHDR_EHSIZE_OFFSET = 0x34, EHDR_PHENTSIZE_OFFSET = 0x36;
static constexpr size_t EHDR_PHNUM_OFFSET = 0x38, EHDR_SHENTSIZE_OFFSET = 0x3a, EHDR_SHNUM_OFFSET = 0x3c;
static constexpr size_t EHDR_SHSTRNDX_OFFSET = 0x3e;

static constexpr uint32_t PT_LOAD_TYPE = 1, PT_DYNAMIC_TYPE = 2, PT_INTERP_TYPE = 3;
static constexpr uint16_t PN_XNUM_VALUE = 0xffff;
static constexpr uint32_t SHT_NULL_TYPE = 0, SHT_SYMTAB_TYPE = 2, SHT_STRTAB_TYPE = 3;
static constexpr uint32_t SHT_NOBITS_TYPE = 8, SHT_SYMTAB_SHNDX_TYPE = 18;
static constexpr uint64_t SHF_COMPRESSED_FLAG = 0x800;

static constexpr uint16_t SHN_UNDEF_INDEX = 0, SHN_LORESERVE_INDEX = 0xff00;
static constexpr uint16_t SHN_ABS_INDEX = 0xfff1, SHN_COMMON_INDEX = 0xfff2, SHN_XINDEX_VALUE = 0xffff;
static constexpr uint8_t SYM_BIND_SHIFT = 4, SYM_TYPE_MASK = 0x0f;

static constexpr size_t DWARF32_HEADER_MIN = 6, DWARF32_VERSION_OFFSET = 4;
static constexpr uint32_t DWARF32_MAX_LENGTH = 0xfffffff0u, DWARF64_SENTINEL = 0xffffffffu;
static constexpr size_t DWARF64_HEADER_MIN = 14, DWARF64_LENGTH_OFFSET = 4, DWARF64_VERSION_OFFSET = 12;
static constexpr uint16_t DWARF_MIN_VERSION = 2, DWARF_MAX_VERSION = 5;

static constexpr uint64_t FOUR_GIB_OFFSET = 0x100000000ull;
static constexpr size_t OFFSET_DIGITS_32 = 8, OFFSET_DIGITS_64 = 16, HEX_HALF_ROW = 8;
static constexpr size_t HEX_CELL_WIDTH = 3, COLUMN_GAP_WIDTH = 2;
static constexpr uint8_t PRINTABLE_ASCII_MIN = 0x20, PRINTABLE_ASCII_MAX = 0x7e;

namespace {

struct header_field_entry {
    std::string_view name;
    uint64_t start;
    uint64_t end;
};

struct code_label_entry {
    uint32_t code;
    const char* label;
};

struct raw_section_header {
    uint32_t sh_name = 0;
    uint32_t sh_type = 0;
    uint64_t sh_flags = 0;
    uint64_t sh_addr = 0;
    uint64_t sh_offset = 0;
    uint64_t sh_size = 0;
    uint32_t sh_link = 0;
    uint32_t sh_info = 0;
    uint64_t sh_entsize = 0;
    byte_range header_range;
};

} // namespace

static constexpr header_field_entry HEADER_FIELD_TABLE[] = {
    { "Ehdr.e_ident", 0x00, 0x10 },     { "Ehdr.e_type", EHDR_TYPE_OFFSET, 0x12 },
    { "Ehdr.e_machine", 0x12, 0x14 },   { "Ehdr.e_version", EHDR_VERSION_OFFSET, 0x18 },
    { "Ehdr.e_entry", 0x18, 0x20 },     { "Ehdr.e_phoff", EHDR_PHOFF_OFFSET, 0x28 },
    { "Ehdr.e_shoff", 0x28, 0x30 },     { "Ehdr.e_flags", EHDR_FLAGS_OFFSET, 0x34 },
    { "Ehdr.e_ehsize", 0x34, 0x36 },    { "Ehdr.e_phentsize", EHDR_PHENTSIZE_OFFSET, 0x38 },
    { "Ehdr.e_phnum", 0x38, 0x3a },     { "Ehdr.e_shentsize", EHDR_SHENTSIZE_OFFSET, 0x3c },
    { "Ehdr.e_shnum", 0x3c, 0x3e },     { "Ehdr.e_shstrndx", EHDR_SHSTRNDX_OFFSET, 0x40 },
};

static constexpr code_label_entry SECTION_TYPE_LABELS[] = {
    { 0, "NULL" },       { 1, "PROGBITS" },  { 2, "SYMTAB" },     { 3, "STRTAB" },
    { 4, "RELA" },       { 5, "HASH" },      { 6, "DYNAMIC" },    { 7, "NOTE" },
    { 8, "NOBITS" },     { 9, "REL" },       { 11, "DYNSYM" },    { 14, "INIT_ARRAY" },
    { 15, "FINI_ARRAY" }, { 18, "SYMTAB_SHNDX" },
};

static constexpr code_label_entry SYMBOL_BINDING_LABELS[] = {
    { ELF_STB_LOCAL, "LOCAL" }, { ELF_STB_GLOBAL, "GLOBAL" }, { ELF_STB_WEAK, "WEAK" },
};

static constexpr code_label_entry SYMBOL_TYPE_LABELS[] = {
    { ELF_STT_NOTYPE, "NOTYPE" },   { ELF_STT_OBJECT, "OBJECT" }, { ELF_STT_FUNC, "FUNC" },
    { ELF_STT_SECTION, "SECTION" }, { ELF_STT_FILE, "FILE" },     { ELF_STT_TLS, "TLS" },
};

static bool checked_range_within(uint64_t offset, uint64_t size, size_t limit) {
    uint64_t bound = static_cast<uint64_t>(limit);
    return offset <= bound && size <= bound - offset;
}

static bool checked_add_u64(uint64_t lhs, uint64_t rhs, uint64_t& sum) {
    if (rhs > UINT64_MAX - lhs) {
        return false;
    }

    sum = lhs + rhs;
    return true;
}

static bool checked_mul_u64(uint64_t lhs, uint64_t rhs, uint64_t& product) {
    if (lhs != 0 && rhs > UINT64_MAX / lhs) {
        return false;
    }

    product = lhs * rhs;
    return true;
}

static uint16_t read_u16_le(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
}

static uint32_t read_u32_le(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint32_t>(read_u16_le(bytes, offset)) | (static_cast<uint32_t>(read_u16_le(bytes, offset + 2)) << 16);
}

static uint64_t read_u64_le(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint64_t>(read_u32_le(bytes, offset)) | (static_cast<uint64_t>(read_u32_le(bytes, offset + 4)) << 32);
}

static bool has_elf_magic(std::span<const uint8_t> bytes) {
    return bytes.size() >= sizeof(ELF_MAGIC_BYTES) && std::memcmp(bytes.data(), ELF_MAGIC_BYTES, sizeof(ELF_MAGIC_BYTES)) == 0;
}

static const char* lookup_code_label(std::span<const code_label_entry> table, uint32_t code, const char* fallback) {
    for (const code_label_entry& entry : table) {
        if (entry.code == code) {
            return entry.label;
        }
    }

    return fallback;
}

static std::string sanitize_utf8_label(const uint8_t* raw, size_t length) {
    std::string cleaned;
    cleaned.reserve(length);
    for (size_t idx = 0; idx < length;) {
        uint8_t lead = raw[idx];
        if (lead >= PRINTABLE_ASCII_MIN && lead <= PRINTABLE_ASCII_MAX) {
            cleaned += static_cast<char>(lead);
            idx++;
            continue;
        }

        size_t seq = (lead & 0xe0u) == 0xc0u ? 2 : ((lead & 0xf0u) == 0xe0u ? 3 : ((lead & 0xf8u) == 0xf0u ? 4 : 0));
        uint32_t cp = seq == 2 ? (lead & 0x1fu) : (seq == 3 ? (lead & 0x0fu) : (lead & 0x07u));
        uint32_t min_cp = seq == 2 ? 0x80u : (seq == 3 ? 0x800u : 0x10000u);
        bool valid = seq > 0 && idx + seq <= length;
        for (size_t k = 1; valid && k < seq; k++) {
            valid = (raw[idx + k] & 0xc0u) == 0x80u;
            cp = (cp << 6) | (raw[idx + k] & 0x3fu);
        }

        if (valid && cp >= min_cp && cp <= 0x10ffffu && (cp < 0xd800u || cp > 0xdfffu)) {
            cleaned.append(reinterpret_cast<const char*>(raw + idx), seq);
            idx += seq;
        } else {
            cleaned += '?';
            idx++;
        }
    }

    return cleaned;
}

static std::string format_rwx_flags(bool readable, bool writable, bool executable) {
    if (!readable) {
        return writable && executable ? "WX" : (writable ? "W" : (executable ? "X" : ""));
    }

    return writable && executable ? "RWX" : (executable ? "R X" : (writable ? "RW" : "R"));
}

static std::string describe_section_flags(uint64_t flags) {
    if ((flags & ELF_SHF_ALLOC) == 0) {
        return format_rwx_flags(false, (flags & ELF_SHF_WRITE) != 0, (flags & ELF_SHF_EXECINSTR) != 0);
    }

    return format_rwx_flags(true, (flags & ELF_SHF_WRITE) != 0, (flags & ELF_SHF_EXECINSTR) != 0);
}

static std::string describe_segment_flags(uint32_t flags) {
    return format_rwx_flags((flags & ELF_PF_R) != 0, (flags & ELF_PF_W) != 0, (flags & ELF_PF_X) != 0);
}

static raw_section_header read_raw_section_header(std::span<const uint8_t> bytes, size_t header_offset) {
    return {
        read_u32_le(bytes, header_offset + 0x00), read_u32_le(bytes, header_offset + 0x04),
        read_u64_le(bytes, header_offset + 0x08), read_u64_le(bytes, header_offset + 0x10),
        read_u64_le(bytes, header_offset + 0x18), read_u64_le(bytes, header_offset + 0x20),
        read_u32_le(bytes, header_offset + 0x28), read_u32_le(bytes, header_offset + 0x2c),
        read_u64_le(bytes, header_offset + 0x38),
        { static_cast<uint64_t>(header_offset), static_cast<uint64_t>(header_offset + ELF64_SHDR_SIZE) },
    };
}

static bool read_string_table_entry(std::span<const uint8_t> bytes, const raw_section_header& strtab,
                                    uint32_t name_offset, std::string& out_name, std::string& error) {
    if (strtab.sh_size == 0 && name_offset == 0) {
        out_name.clear();
        return true;
    }

    if (static_cast<uint64_t>(name_offset) >= strtab.sh_size) {
        error = "ELF string table offset is past string table end";
        return false;
    }

    size_t start = static_cast<size_t>(strtab.sh_offset + static_cast<uint64_t>(name_offset));
    size_t max_len = static_cast<size_t>(strtab.sh_size - static_cast<uint64_t>(name_offset));
    const void* terminator = std::memchr(bytes.data() + start, '\0', max_len);
    if (terminator == nullptr) {
        error = "ELF string table is missing a NUL terminator";
        return false;
    }

    size_t length = static_cast<size_t>(static_cast<const uint8_t*>(terminator) - (bytes.data() + start));
    out_name = sanitize_utf8_label(bytes.data() + start, length);
    return true;
}

static uint16_t read_dwarf_unit_version(std::span<const uint8_t> section_bytes) {
    if (section_bytes.size() < DWARF32_HEADER_MIN) {
        return 0;
    }

    uint32_t initial_length = read_u32_le(section_bytes, 0);
    if (initial_length < DWARF32_MAX_LENGTH) {
        if (initial_length < sizeof(uint16_t) ||
            !checked_range_within(DWARF32_VERSION_OFFSET, initial_length, section_bytes.size())) {
            return 0;
        }

        uint16_t version = read_u16_le(section_bytes, DWARF32_VERSION_OFFSET);
        return (version >= DWARF_MIN_VERSION && version <= DWARF_MAX_VERSION) ? version : 0;
    }

    if (initial_length != DWARF64_SENTINEL || section_bytes.size() < DWARF64_HEADER_MIN) {
        return 0;
    }

    uint64_t length64 = read_u64_le(section_bytes, DWARF64_LENGTH_OFFSET);
    if (length64 < sizeof(uint16_t) ||
        !checked_range_within(DWARF64_VERSION_OFFSET, length64, section_bytes.size())) {
        return 0;
    }

    uint16_t version = read_u16_le(section_bytes, DWARF64_VERSION_OFFSET);
    return (version >= DWARF_MIN_VERSION && version <= DWARF_MAX_VERSION) ? version : 0;
}

static bool resolve_header_counts(std::span<const uint8_t> bytes, uint64_t phoff, uint16_t raw_phnum,
                                  uint64_t shoff, uint16_t shentsize, uint16_t raw_shnum,
                                  uint16_t raw_shstrndx, uint32_t& out_phnum, uint32_t& out_shnum,
                                  uint32_t& out_shstrndx, std::string& error) {
    bool needs_section_zero = (raw_phnum == PN_XNUM_VALUE) ||
                              (raw_shstrndx == SHN_XINDEX_VALUE) ||
                              (raw_shnum == 0 && shoff != 0);
    raw_section_header section_zero;
    if (needs_section_zero) {
        if (shoff == 0 || shentsize < ELF64_SHDR_SIZE || !checked_range_within(shoff, shentsize, bytes.size())) {
            error = "ELF extended header count requires a valid section header 0";
            return false;
        }

        section_zero = read_raw_section_header(bytes, static_cast<size_t>(shoff));
    }

    uint64_t resolved_shnum = raw_shnum;
    if (raw_shnum == 0 && shoff != 0) {
        uint64_t ext_table_bytes = 0;
        if (section_zero.sh_size == 0 ||
            !checked_mul_u64(section_zero.sh_size, shentsize, ext_table_bytes) ||
            !checked_range_within(shoff, ext_table_bytes, bytes.size()) ||
            ext_table_bytes / shentsize > UINT32_MAX) {
            error = "ELF extended section count in section header 0 is invalid";
            return false;
        }

        resolved_shnum = section_zero.sh_size;
    }

    uint32_t resolved_phnum = raw_phnum == PN_XNUM_VALUE ? section_zero.sh_info : raw_phnum;
    if (resolved_shnum > ELF_MAX_SECTIONS || resolved_phnum > ELF_MAX_PROGRAM_HEADERS) {
        error = "ELF header table count exceeds supported maximum";
        return false;
    }

    if ((resolved_shnum > 0 && shoff == 0) || (resolved_phnum > 0 && phoff == 0)) {
        error = "ELF header declares non-empty table with zero file offset";
        return false;
    }

    out_phnum = resolved_phnum;
    out_shnum = static_cast<uint32_t>(resolved_shnum);
    out_shstrndx = raw_shstrndx == SHN_XINDEX_VALUE ? section_zero.sh_link : raw_shstrndx;
    return true;
}

static bool parse_program_headers(std::span<const uint8_t> bytes, uint64_t phoff, uint16_t phentsize,
                                  uint32_t phnum, elf_binary_model& model, std::string& error) {
    if (phnum == 0) {
        return true;
    }

    uint64_t table_bytes = 0;
    if (phentsize < ELF64_PHDR_SIZE || !checked_mul_u64(phnum, phentsize, table_bytes) ||
        !checked_range_within(phoff, table_bytes, bytes.size())) {
        error = "ELF program header table is invalid or exceeds file bounds";
        return false;
    }

    for (uint32_t index = 0; index < phnum; index++) {
        size_t off = static_cast<size_t>(phoff + static_cast<uint64_t>(index) * phentsize);
        uint32_t p_type = read_u32_le(bytes, off + 0x00);
        uint32_t p_flags = read_u32_le(bytes, off + 0x04);
        uint64_t p_offset = read_u64_le(bytes, off + 0x08);
        uint64_t p_vaddr = read_u64_le(bytes, off + 0x10);
        uint64_t p_paddr = read_u64_le(bytes, off + 0x18);

        uint64_t p_filesz = read_u64_le(bytes, off + 0x20);
        uint64_t p_memsz = read_u64_le(bytes, off + 0x28);
        uint64_t p_align = read_u64_le(bytes, off + 0x30);

        if (p_type == PT_INTERP_TYPE || p_type == PT_DYNAMIC_TYPE) {
            model.header.is_static = false;
        }

        if (p_filesz > 0 && !checked_range_within(p_offset, p_filesz, bytes.size())) {
            error = "ELF program segment payload exceeds file bounds";
            return false;
        }

        if (p_type != PT_LOAD_TYPE) {
            continue;
        }

        uint64_t vaddr_end = 0;
        if (p_filesz > p_memsz || !checked_add_u64(p_vaddr, p_memsz, vaddr_end)) {
            error = "ELF PT_LOAD segment has invalid memory bounds";
            return false;
        }

        model.load_segments.push_back({
            index, p_vaddr, p_paddr, p_offset, p_filesz, p_memsz, p_align, p_flags,
            describe_segment_flags(p_flags),
            { static_cast<uint64_t>(off), static_cast<uint64_t>(off + ELF64_PHDR_SIZE) },
            { p_offset, p_offset + p_filesz },
        });
    }

    return true;
}

static bool parse_symbol_table_section(std::span<const uint8_t> bytes, uint32_t symtab_index,
                                       const std::vector<raw_section_header>& raw_sections,
                                       const std::vector<elf_section>& sections,
                                       std::unordered_map<uint64_t, std::string>& demangle_cache,
                                       std::vector<elf_symbol>& symbols, std::string& error) {
    const raw_section_header& symtab = raw_sections[symtab_index];
    if (symtab.sh_size == 0) {
        return true;
    }

    if (symtab.sh_entsize < ELF64_SYM_SIZE || (symtab.sh_size % symtab.sh_entsize) != 0) {
        error = "ELF symbol table has an invalid entry size";
        return false;
    }

    if (symtab.sh_link >= raw_sections.size() || raw_sections[symtab.sh_link].sh_type != SHT_STRTAB_TYPE) {
        error = "ELF symbol table links to an invalid string table section";
        return false;
    }

    const raw_section_header& strtab = raw_sections[symtab.sh_link];
    uint64_t symbol_count = symtab.sh_size / symtab.sh_entsize;
    if (symbol_count > ELF_MAX_SYMBOLS + 1 || symbols.size() > ELF_MAX_SYMBOLS) {
        error = "ELF symbol count exceeds maximum supported symbols";
        return false;
    }

    const raw_section_header* shndx_table = nullptr;
    for (const raw_section_header& candidate : raw_sections) {
        if (candidate.sh_type == SHT_SYMTAB_SHNDX_TYPE && candidate.sh_link == symtab_index) {
            uint64_t expected_bytes = 0;
            if (!checked_mul_u64(symbol_count, ELF64_SHNDX_ENTRY_SIZE, expected_bytes) ||
                candidate.sh_size < expected_bytes) {
                error = "ELF SHT_SYMTAB_SHNDX section is truncated";
                return false;
            }

            shndx_table = &candidate;
            break;
        }
    }

    for (uint64_t sym_idx = 1; sym_idx < symbol_count; sym_idx++) {
        if (symbols.size() >= ELF_MAX_SYMBOLS) {
            error = "ELF symbol count exceeds maximum supported symbols";
            return false;
        }

        size_t off = static_cast<size_t>(symtab.sh_offset + sym_idx * symtab.sh_entsize);
        uint32_t st_name = read_u32_le(bytes, off + 0x00);
        uint8_t st_info = bytes[off + 0x04];
        uint16_t st_shndx = read_u16_le(bytes, off + 0x06);
        uint64_t st_value = read_u64_le(bytes, off + 0x08);
        uint64_t st_size = read_u64_le(bytes, off + 0x10);

        uint64_t end_vma = 0;
        if (!checked_add_u64(st_value, st_size, end_vma)) {
            error = "ELF symbol address plus size overflows uint64_t";
            return false;
        }

        std::string raw_name;
        if (!read_string_table_entry(bytes, strtab, st_name, raw_name, error)) {
            return false;
        }

        if (raw_name.empty()) {
            continue;
        }

        uint32_t resolved_shndx = st_shndx;
        std::string section_name;
        if (st_shndx == SHN_UNDEF_INDEX) {
            section_name = "UND";
        } else if (st_shndx == SHN_ABS_INDEX) {
            section_name = "ABS";
        } else if (st_shndx == SHN_COMMON_INDEX) {
            section_name = "COM";
        } else if (st_shndx == SHN_XINDEX_VALUE) {
            if (shndx_table == nullptr) {
                error = "ELF symbol uses SHN_XINDEX without SHT_SYMTAB_SHNDX";
                return false;
            }

            resolved_shndx = read_u32_le(bytes, static_cast<size_t>(shndx_table->sh_offset + sym_idx * 4));
            if (resolved_shndx >= sections.size()) {
                error = "ELF extended symbol section index is out of range";
                return false;
            }

            section_name = sections[resolved_shndx].name;
        } else if (st_shndx >= SHN_LORESERVE_INDEX) {
            section_name = "RSV";
        } else if (st_shndx >= sections.size()) {
            error = "ELF symbol section index is out of range";
            return false;
        } else {
            section_name = sections[st_shndx].name;
        }

        uint64_t cache_key = (static_cast<uint64_t>(symtab.sh_link) << 32) | st_name;
        auto cached = demangle_cache.find(cache_key);
        std::string demangled = cached != demangle_cache.end()
                                    ? cached->second
                                    : demangle_cache.emplace(cache_key, demangle_elf_symbol(raw_name)).first->second;

        uint8_t binding = static_cast<uint8_t>(st_info >> SYM_BIND_SHIFT);
        uint8_t symbol_type = static_cast<uint8_t>(st_info & SYM_TYPE_MASK);
        symbols.push_back({
            std::move(raw_name), std::move(demangled), std::move(section_name),
            resolved_shndx, st_value, st_size, binding,
            lookup_code_label(SYMBOL_BINDING_LABELS, binding, "OTHER"),
            symbol_type, lookup_code_label(SYMBOL_TYPE_LABELS, symbol_type, "OTHER"),
        });
    }

    return true;
}

static bool parse_section_headers(std::span<const uint8_t> bytes, uint64_t shoff, uint16_t shentsize,
                                  uint32_t shnum, uint32_t shstrndx, elf_binary_model& model,
                                  std::string& error) {
    if (shnum == 0) {
        return true;
    }

    uint64_t table_bytes = 0;
    if (shentsize < ELF64_SHDR_SIZE || !checked_mul_u64(shnum, shentsize, table_bytes) ||
        !checked_range_within(shoff, table_bytes, bytes.size())) {
        error = "ELF section header table is invalid or exceeds file bounds";
        return false;
    }

    std::vector<raw_section_header> raw_sections;
    raw_sections.reserve(shnum);
    for (uint32_t index = 0; index < shnum; index++) {
        raw_section_header raw = read_raw_section_header(bytes, static_cast<size_t>(shoff + index * shentsize));
        uint64_t end_vma = 0;
        if (raw.sh_type != SHT_NULL_TYPE && !checked_add_u64(raw.sh_addr, raw.sh_size, end_vma)) {
            error = "ELF section virtual address range overflows uint64_t";
            return false;
        }

        if (raw.sh_type != SHT_NULL_TYPE && raw.sh_type != SHT_NOBITS_TYPE && raw.sh_size > 0 &&
            !checked_range_within(raw.sh_offset, raw.sh_size, bytes.size())) {
            error = "ELF section payload exceeds file bounds";
            return false;
        }

        raw_sections.push_back(raw);
    }

    const raw_section_header* shstrtab = nullptr;
    if (shstrndx != SHN_UNDEF_INDEX) {
        if (shstrndx >= shnum || raw_sections[shstrndx].sh_type != SHT_STRTAB_TYPE) {
            error = "ELF section header string table index is invalid";
            return false;
        }

        shstrtab = &raw_sections[shstrndx];
    }

    uint64_t total_file_backed_bytes = 0;
    model.sections.reserve(shnum);
    for (uint32_t index = 0; index < shnum; index++) {
        const raw_section_header& raw = raw_sections[index];
        std::string name;
        if (shstrtab != nullptr && !read_string_table_entry(bytes, *shstrtab, raw.sh_name, name, error)) {
            return false;
        }

        bool is_nobits = raw.sh_type == SHT_NOBITS_TYPE;
        bool has_file_bytes = raw.sh_type != SHT_NULL_TYPE && !is_nobits;
        uint64_t sec_size = raw.sh_type == SHT_NULL_TYPE ? 0 : raw.sh_size;

        if (name.starts_with(".debug_") || name.starts_with(".zdebug_")) {
            model.header.has_dwarf = true;
            if (has_file_bytes && (raw.sh_flags & SHF_COMPRESSED_FLAG) == 0 &&
                (name == ".debug_info" || name == ".debug_line") && raw.sh_size > 0) {
                auto slice = bytes.subspan(static_cast<size_t>(raw.sh_offset), static_cast<size_t>(raw.sh_size));
                model.header.dwarf_version = std::max(model.header.dwarf_version, read_dwarf_unit_version(slice));
            }
        }

        if (has_file_bytes && !checked_add_u64(total_file_backed_bytes, sec_size, total_file_backed_bytes)) {
            error = "ELF total section size overflows uint64_t";
            return false;
        }

        model.sections.push_back({
            index, std::move(name), raw.sh_type, lookup_code_label(SECTION_TYPE_LABELS, raw.sh_type, "OTHER"),
            raw.sh_addr, raw.sh_offset, sec_size, 0.0, raw.sh_flags,
            describe_section_flags(raw.sh_flags), is_nobits, raw.header_range,
            has_file_bytes ? byte_range{ raw.sh_offset, raw.sh_offset + raw.sh_size } : byte_range{ 0, 0 },
        });
    }

    if (total_file_backed_bytes > 0) {
        double denom = static_cast<double>(total_file_backed_bytes);
        for (elf_section& section : model.sections) {
            if (section.type != SHT_NULL_TYPE && !section.is_nobits) {
                section.share = static_cast<double>(section.size) / denom;
            }
        }
    }

    std::set<std::pair<uint64_t, uint64_t>> seen_symtab_ranges;
    std::unordered_map<uint64_t, std::string> demangle_cache;
    for (uint32_t index = 0; index < shnum; index++) {
        if (raw_sections[index].sh_type == SHT_SYMTAB_TYPE &&
            seen_symtab_ranges.insert({ raw_sections[index].sh_offset, raw_sections[index].sh_size }).second) {
            if (!parse_symbol_table_section(bytes, index, raw_sections, model.sections,
                                            demangle_cache, model.symbols, error)) {
                return false;
            }
        }
    }

    return true;
}

static size_t hex_byte_column(size_t offset_digits, size_t byte_in_row) {
    return offset_digits + COLUMN_GAP_WIDTH + byte_in_row * HEX_CELL_WIDTH + (byte_in_row >= HEX_HALF_ROW ? 1 : 0);
}

static size_t ascii_start_column(size_t offset_digits) {
    return hex_byte_column(offset_digits, HEX_BYTES_PER_ROW - 1) + 2 + COLUMN_GAP_WIDTH + 1;
}

bool is_elf64_binary(std::span<const uint8_t> header_bytes) {
    if (header_bytes.size() <= EI_VERSION_OFFSET || !has_elf_magic(header_bytes)) {
        return false;
    }

    if (header_bytes[EI_CLASS_OFFSET] != ELF_CLASS_64 ||
        header_bytes[EI_DATA_OFFSET] != ELF_DATA_LSB ||
        header_bytes[EI_VERSION_OFFSET] != ELF_CURRENT_VERSION) {
        return false;
    }

    if (header_bytes.size() >= MIN_TYPED_PREFIX_BYTES) {
        uint16_t type = read_u16_le(header_bytes, EHDR_TYPE_OFFSET);
        uint16_t machine = read_u16_le(header_bytes, EHDR_MACHINE_OFFSET);
        return (type == ELF_TYPE_REL || type == ELF_TYPE_EXEC || type == ELF_TYPE_DYN) &&
               (machine == ELF_MACHINE_X86_64 || machine == ELF_MACHINE_AARCH64);
    }

    return true;
}

byte_range elf_header_field_range(std::string_view field_name) {
    for (const header_field_entry& entry : HEADER_FIELD_TABLE) {
        if (entry.name == field_name) {
            return { entry.start, entry.end };
        }
    }

    return { 0, 0 };
}

std::string demangle_elf_symbol(std::string_view raw_name) {
    if (!raw_name.starts_with("_Z") || raw_name.size() > ELF_MAX_DEMANGLE_INPUT) {
        return std::string(raw_name);
    }

#if SHEEN_HAS_CXA_DEMANGLE
    std::string null_terminated(raw_name);
    int status = 0;
    char* demangled = abi::__cxa_demangle(null_terminated.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled != nullptr) {
        size_t out_len = std::strlen(demangled);
        std::string result = out_len <= ELF_MAX_DEMANGLE_OUTPUT
                                 ? std::string(demangled, out_len)
                                 : std::string(raw_name);
        std::free(demangled);
        return result;
    }

    std::free(demangled);
#endif

    return std::string(raw_name);
}

bool symbol_file_byte_range(const elf_binary_model& model, const elf_symbol& symbol, byte_range& out_range) {
    if (symbol.section_index == 0 || symbol.section_index >= model.sections.size()) {
        return false;
    }

    const elf_section& section = model.sections[symbol.section_index];
    if (section.is_nobits || section.size == 0) {
        return false;
    }

    uint64_t relative = 0;
    if (model.header.type == ELF_TYPE_REL) {
        if (symbol.vma >= section.size) {
            return false;
        }

        relative = symbol.vma;
    } else {
        if (symbol.vma < section.vma || symbol.vma >= section.vma + section.size) {
            return false;
        }

        relative = symbol.vma - section.vma;
    }

    uint64_t file_offset = section.file_offset + relative;
    uint64_t length = std::max<uint64_t>(1, std::min<uint64_t>(symbol.size, section.size - relative));
    out_range = { file_offset, file_offset + length };
    return true;
}

bool parse_elf_binary(std::span<const uint8_t> bytes, elf_binary_model& model, std::string& error) {
    model = {};
    error.clear();

    if (bytes.size() < ELF64_HEADER_SIZE || !has_elf_magic(bytes)) {
        error = "input is not an ELF binary";
        return false;
    }

    if (bytes[EI_CLASS_OFFSET] != ELF_CLASS_64 || bytes[EI_DATA_OFFSET] != ELF_DATA_LSB) {
        error = "ELF binary is not 64-bit little-endian";
        return false;
    }

    if (bytes[EI_VERSION_OFFSET] != ELF_CURRENT_VERSION ||
        read_u32_le(bytes, EHDR_VERSION_OFFSET) != ELF_CURRENT_VERSION) {
        error = "ELF binary has an unsupported format version";
        return false;
    }

    uint16_t ehsize = read_u16_le(bytes, EHDR_EHSIZE_OFFSET);
    uint16_t type = read_u16_le(bytes, EHDR_TYPE_OFFSET);
    uint16_t machine = read_u16_le(bytes, EHDR_MACHINE_OFFSET);
    if (ehsize < ELF64_HEADER_SIZE || ehsize > bytes.size()) {
        error = "ELF header size is invalid or exceeds file size";
        return false;
    }

    if ((type != ELF_TYPE_REL && type != ELF_TYPE_EXEC && type != ELF_TYPE_DYN) ||
        (machine != ELF_MACHINE_X86_64 && machine != ELF_MACHINE_AARCH64)) {
        error = "ELF binary type or machine architecture is unsupported";
        return false;
    }

    uint64_t entry = read_u64_le(bytes, EHDR_ENTRY_OFFSET);
    uint64_t phoff = read_u64_le(bytes, EHDR_PHOFF_OFFSET);
    uint64_t shoff = read_u64_le(bytes, EHDR_SHOFF_OFFSET);
    uint16_t phentsize = read_u16_le(bytes, EHDR_PHENTSIZE_OFFSET);
    uint16_t shentsize = read_u16_le(bytes, EHDR_SHENTSIZE_OFFSET);

    uint32_t phnum = 0;
    uint32_t shnum = 0;
    uint32_t shstrndx = 0;
    if (!resolve_header_counts(bytes, phoff, read_u16_le(bytes, EHDR_PHNUM_OFFSET), shoff, shentsize,
                               read_u16_le(bytes, EHDR_SHNUM_OFFSET), read_u16_le(bytes, EHDR_SHSTRNDX_OFFSET),
                               phnum, shnum, shstrndx, error)) {
        return false;
    }

    const char* type_str = type == ELF_TYPE_REL ? "ET_REL" : (type == ELF_TYPE_EXEC ? "ET_EXEC" : "ET_DYN");
    model.header = {
        type, type_str, machine,
        machine == ELF_MACHINE_X86_64 ? "EM_X86_64" : "EM_AARCH64",
        machine == ELF_MACHINE_X86_64 ? "x86_64" : "aarch64",
        entry, phoff, shoff, phnum, shnum, true, false, 0, { 0, ehsize },
    };

    model.header_fields.reserve(std::size(HEADER_FIELD_TABLE));
    for (const header_field_entry& field : HEADER_FIELD_TABLE) {
        model.header_fields.push_back({ std::string(field.name), { field.start, field.end } });
    }

    if (!parse_program_headers(bytes, phoff, phentsize, phnum, model, error) ||
        !parse_section_headers(bytes, shoff, shentsize, shnum, shstrndx, model, error)) {
        model = {};
        return false;
    }

    return true;
}

hex_ascii_window format_hex_ascii_window(std::span<const uint8_t> bytes, uint64_t base_offset,
                                         size_t row_count, byte_range selection) {
    hex_ascii_window window{ base_offset, selection, {}, {} };
    uint64_t file_size = static_cast<uint64_t>(bytes.size());
    if (row_count == 0 || base_offset >= file_size) {
        return window;
    }

    uint64_t available_rows = (file_size - base_offset + HEX_BYTES_PER_ROW - 1) / HEX_BYTES_PER_ROW;
    size_t actual_rows = static_cast<size_t>(std::min<uint64_t>(row_count, available_rows));
    uint64_t last_shown_offset = base_offset + static_cast<uint64_t>(actual_rows - 1) * HEX_BYTES_PER_ROW;
    size_t offset_digits = last_shown_offset >= FOUR_GIB_OFFSET ? OFFSET_DIGITS_64 : OFFSET_DIGITS_32;
    size_t ascii_col_base = ascii_start_column(offset_digits);

    window.rows.reserve(actual_rows);
    for (size_t row_idx = 0; row_idx < actual_rows; row_idx++) {
        uint64_t row_offset = base_offset + static_cast<uint64_t>(row_idx) * HEX_BYTES_PER_ROW;
        size_t count_in_row = static_cast<size_t>(std::min<uint64_t>(HEX_BYTES_PER_ROW, file_size - row_offset));

        std::string line(ascii_col_base + HEX_BYTES_PER_ROW + 1, ' ');
        char offset_buf[OFFSET_DIGITS_64 + 1] = {};
        std::snprintf(offset_buf, sizeof(offset_buf), "%0*llx",
                      static_cast<int>(offset_digits), static_cast<unsigned long long>(row_offset));
        std::memcpy(line.data(), offset_buf, offset_digits);

        line[ascii_col_base - 1] = '|';
        for (size_t byte_idx = 0; byte_idx < count_in_row; byte_idx++) {
            uint8_t byte_value = bytes[static_cast<size_t>(row_offset) + byte_idx];
            char hex_pair[3] = {};
            std::snprintf(hex_pair, sizeof(hex_pair), "%02x", static_cast<unsigned>(byte_value));

            size_t hex_col = hex_byte_column(offset_digits, byte_idx);
            line[hex_col] = hex_pair[0];
            line[hex_col + 1] = hex_pair[1];
            bool printable = byte_value >= PRINTABLE_ASCII_MIN && byte_value <= PRINTABLE_ASCII_MAX;
            line[ascii_col_base + byte_idx] = printable ? static_cast<char>(byte_value) : '.';
        }

        line[ascii_col_base + count_in_row] = '|';
        line.resize(ascii_col_base + count_in_row + 1);

        hex_ascii_row formatted_row;
        formatted_row.offset = row_offset;
        formatted_row.byte_count = count_in_row;

        if (selection.start < selection.end) {
            uint64_t overlap_start = std::max(row_offset, selection.start);
            uint64_t overlap_end = std::min(row_offset + count_in_row, selection.end);
            if (overlap_start < overlap_end) {
                size_t first_sel = static_cast<size_t>(overlap_start - row_offset);
                size_t end_sel = static_cast<size_t>(overlap_end - row_offset);
                formatted_row.has_selection = true;
                formatted_row.selection_first_byte = first_sel;
                formatted_row.selection_end_byte = end_sel;

                formatted_row.hex_column_start = hex_byte_column(offset_digits, first_sel);
                formatted_row.hex_column_end = hex_byte_column(offset_digits, end_sel - 1) + 2;
                formatted_row.ascii_column_start = ascii_col_base + first_sel;
                formatted_row.ascii_column_end = ascii_col_base + end_sel;
            }
        }

        if (!window.text.empty()) {
            window.text += '\n';
        }

        window.text += line;
        formatted_row.line = std::move(line);
        window.rows.push_back(std::move(formatted_row));
    }

    return window;
}

} // namespace sheen
