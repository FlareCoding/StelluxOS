#ifndef SHEEN_EXTENSIONS_PERF_BINARIES_ELF_MODEL_H
#define SHEEN_EXTENSIONS_PERF_BINARIES_ELF_MODEL_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sheen {

inline constexpr uint16_t ELF_TYPE_REL = 1, ELF_TYPE_EXEC = 2, ELF_TYPE_DYN = 3;
inline constexpr uint16_t ELF_MACHINE_X86_64 = 62, ELF_MACHINE_AARCH64 = 183;
inline constexpr uint64_t ELF_SHF_WRITE = 0x1, ELF_SHF_ALLOC = 0x2, ELF_SHF_EXECINSTR = 0x4;
inline constexpr uint32_t ELF_PF_X = 0x1, ELF_PF_W = 0x2, ELF_PF_R = 0x4;
inline constexpr uint8_t ELF_STB_LOCAL = 0, ELF_STB_GLOBAL = 1, ELF_STB_WEAK = 2;
inline constexpr uint8_t ELF_STT_NOTYPE = 0, ELF_STT_OBJECT = 1, ELF_STT_FUNC = 2;
inline constexpr uint8_t ELF_STT_SECTION = 3, ELF_STT_FILE = 4, ELF_STT_TLS = 6;

inline constexpr size_t HEX_BYTES_PER_ROW = 16;
inline constexpr uint32_t ELF_MAX_PROGRAM_HEADERS = 4096;
inline constexpr uint32_t ELF_MAX_SECTIONS = 16384;
inline constexpr size_t ELF_MAX_SYMBOLS = 262144;
inline constexpr size_t ELF_MAX_DEMANGLE_INPUT = 1024;
inline constexpr size_t ELF_MAX_DEMANGLE_OUTPUT = 4096;

/* Half-open file byte interval `[start, end)` within an ELF image. */
struct byte_range {
    uint64_t start = 0;
    uint64_t end = 0;
};

/* Named header field paired with its file byte interval, such as `Ehdr.e_entry` at `[0x18, 0x20)`. */
struct elf_field_range {
    std::string name;
    byte_range range;
};

struct elf_header_summary {
    uint16_t type = 0;
    std::string type_name;
    uint16_t machine = 0;
    std::string machine_name, arch_name;
    uint64_t entry = 0, phoff = 0, shoff = 0;
    uint32_t phnum = 0, shnum = 0;
    bool is_static = true, has_dwarf = false;
    uint16_t dwarf_version = 0;
    byte_range header_byte_range;
};

struct elf_section {
    uint32_t index = 0;
    std::string name;
    uint32_t type = 0;
    std::string type_name;
    uint64_t vma = 0, file_offset = 0, size = 0;
    double share = 0.0;
    uint64_t flags = 0;
    std::string flags_label;
    bool is_nobits = false;
    byte_range header_byte_range, content_byte_range;
};

struct elf_segment {
    uint32_t index = 0;
    uint64_t vaddr = 0, paddr = 0, file_offset = 0;
    uint64_t file_size = 0, mem_size = 0, align = 0;
    uint32_t flags = 0;
    std::string flags_label;
    byte_range header_byte_range, file_byte_range;
};

struct elf_symbol {
    std::string raw_name, demangled_name, section_name;
    uint32_t section_index = 0;
    uint64_t vma = 0, size = 0;
    uint8_t binding = 0;
    std::string binding_name;
    uint8_t symbol_type = 0;
    std::string type_name;
};

struct elf_binary_model {
    elf_header_summary header;
    std::vector<elf_field_range> header_fields;
    std::vector<elf_section> sections;
    std::vector<elf_segment> load_segments;
    std::vector<elf_symbol> symbols;
};

struct hex_ascii_row {
    uint64_t offset = 0;
    size_t byte_count = 0;
    std::string line;
    bool has_selection = false;
    size_t selection_first_byte = 0;
    size_t selection_end_byte = 0;
    size_t hex_column_start = 0;
    size_t hex_column_end = 0;
    size_t ascii_column_start = 0;
    size_t ascii_column_end = 0;
};

struct hex_ascii_window {
    uint64_t base_offset = 0;
    byte_range selection;
    std::vector<hex_ascii_row> rows;
    std::string text;
};

bool is_elf64_binary(std::span<const uint8_t> header_bytes);
bool parse_elf_binary(std::span<const uint8_t> bytes, elf_binary_model& model, std::string& error);
byte_range elf_header_field_range(std::string_view field_name);
std::string demangle_elf_symbol(std::string_view raw_name);
bool symbol_file_byte_range(const elf_binary_model& model, const elf_symbol& symbol, byte_range& out_range);
hex_ascii_window format_hex_ascii_window(std::span<const uint8_t> bytes, uint64_t base_offset,
                                         size_t row_count, byte_range selection = {});

} // namespace sheen

#endif // SHEEN_EXTENSIONS_PERF_BINARIES_ELF_MODEL_H
