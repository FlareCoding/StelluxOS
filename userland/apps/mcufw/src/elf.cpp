#include "elf.hpp"

#include "files.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>

// Field offsets in the ELF header and in a section header, as the ELF specification lays out 32-bit files
constexpr size_t ELF_HEADER_SIZE          = 52;
constexpr size_t CLASS_OFFSET             = 4;
constexpr size_t BYTE_ORDER_OFFSET        = 5;
constexpr size_t SECTION_TABLE_OFFSET     = 32;
constexpr size_t SECTION_ENTRY_OFFSET     = 46;
constexpr size_t SECTION_COUNT_OFFSET     = 48;
constexpr size_t SECTION_HEADER_SIZE      = 40;
constexpr size_t SECTION_FLAGS_OFFSET     = 8;
constexpr size_t SECTION_ADDRESS_OFFSET   = 12;
constexpr size_t SECTION_SIZE_OFFSET      = 20;

constexpr std::string_view ELF_MAGIC          = "\x7f" "ELF";
constexpr uint8_t          CLASS_32_BIT       = 1;
constexpr uint8_t          DATA_LITTLE_ENDIAN = 1;
constexpr uint32_t         SECTION_EXECUTABLE = 0x4;

static uint16_t read_u16(const std::string& bytes, size_t offset) {
    return static_cast<uint16_t>(static_cast<uint8_t>(bytes[offset]) |
                                 static_cast<uint8_t>(bytes[offset + 1]) << 8);
}

static uint32_t read_u32(const std::string& bytes, size_t offset) {
    return static_cast<uint32_t>(read_u16(bytes, offset)) | static_cast<uint32_t>(read_u16(bytes, offset + 2)) << 16;
}

bool read_executable_ranges(const std::string& path, std::vector<address_range>& ranges, std::string& error) {
    std::string bytes;
    if (!read_file(path, bytes, error)) {
        return false;
    }

    if (bytes.size() < ELF_HEADER_SIZE || bytes.compare(0, ELF_MAGIC.size(), ELF_MAGIC) != 0) {
        error = path + " is not an ELF file";
        return false;
    }

    bool is_32_bit = static_cast<uint8_t>(bytes[CLASS_OFFSET]) == CLASS_32_BIT;
    bool is_little_endian = static_cast<uint8_t>(bytes[BYTE_ORDER_OFFSET]) == DATA_LITTLE_ENDIAN;
    if (!is_32_bit || !is_little_endian) {
        error = path + " is not a 32-bit little-endian ELF file";
        return false;
    }

    uint64_t table = read_u32(bytes, SECTION_TABLE_OFFSET);
    uint64_t entry_size = read_u16(bytes, SECTION_ENTRY_OFFSET);
    uint64_t count = read_u16(bytes, SECTION_COUNT_OFFSET);
    if (count > 0 && (entry_size < SECTION_HEADER_SIZE || table + count * entry_size > bytes.size())) {
        error = path + " has a section table past its end";
        return false;
    }

    ranges.clear();
    for (uint64_t index = 0; index < count; index++) {
        size_t header = static_cast<size_t>(table + index * entry_size);
        if ((read_u32(bytes, header + SECTION_FLAGS_OFFSET) & SECTION_EXECUTABLE) == 0) {
            continue;
        }

        uint32_t start = read_u32(bytes, header + SECTION_ADDRESS_OFFSET);
        ranges.push_back({ start, start + read_u32(bytes, header + SECTION_SIZE_OFFSET) });
    }

    std::sort(ranges.begin(), ranges.end(), [](const address_range& a, const address_range& b) {
        return a.start < b.start;
    });

    return true;
}
