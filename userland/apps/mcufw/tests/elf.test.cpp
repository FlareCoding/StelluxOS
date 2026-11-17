/* read_executable_ranges, run on the build machine against ELF files the test writes: the executable sections by start
 * address, and the files it refuses */

#include "elf.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

struct section {
    uint32_t flags;
    uint32_t address;
    uint32_t size;
};

// The ELF32 layout restated from the specification, so the test checks the reader independently
constexpr size_t   ELF_HEADER_SIZE           = 52;
constexpr size_t   SECTION_ENTRY_SIZE        = 40;
constexpr size_t   MAGIC_SIZE                = 4;
constexpr size_t   MAGIC_SECOND_OFFSET       = 1;
constexpr size_t   CLASS_OFFSET              = 4;
constexpr size_t   BYTE_ORDER_OFFSET         = 5;
constexpr size_t   VERSION_OFFSET            = 6;
constexpr size_t   SECTION_TABLE_OFFSET      = 32;
constexpr size_t   HEADER_SIZE_OFFSET        = 40;
constexpr size_t   SECTION_ENTRY_SIZE_OFFSET = 46;
constexpr size_t   SECTION_COUNT_OFFSET      = 48;
constexpr size_t   SECTION_FLAGS_OFFSET      = 8;
constexpr size_t   SECTION_ADDRESS_OFFSET    = 12;
constexpr size_t   SECTION_SIZE_OFFSET       = 20;
constexpr char     CLASS_32_BIT              = 1;
constexpr char     CLASS_64_BIT              = 2;
constexpr char     LITTLE_ENDIAN_DATA        = 1;
constexpr char     BIG_ENDIAN_DATA           = 2;
constexpr char     CURRENT_VERSION           = 1;
constexpr uint32_t ALLOCATED                 = 0x2;
constexpr uint32_t ALLOCATED_CODE            = 0x6;
constexpr uint32_t WRITABLE_DATA             = 0x3;
constexpr uint32_t MAX_REPORTS               = 8;

static const section ESP32_SECTIONS[] = {
    { 0, 0, 0 },
    { ALLOCATED_CODE, 0x400d0020, 0x20000 },
    { WRITABLE_DATA, 0x3ffb0000, 0x1000 },
    { ALLOCATED_CODE, 0x40080000, 0x400 },
    { ALLOCATED, 0x3f400020, 0x8000 },
    { ALLOCATED_CODE, 0x40080400, 0x9000 },
    { 0, 0, 0x30000 },
};

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static void put_u16(std::string& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<char>(value & 0xff);
    bytes[offset + 1] = static_cast<char>(value >> 8);
}

static void put_u32(std::string& bytes, size_t offset, uint32_t value) {
    put_u16(bytes, offset, static_cast<uint16_t>(value & 0xffff));
    put_u16(bytes, offset + 2, static_cast<uint16_t>(value >> 16));
}

static std::string make_elf(const section* sections, size_t count) {
    std::string bytes(ELF_HEADER_SIZE + count * SECTION_ENTRY_SIZE, '\0');
    bytes.replace(0, MAGIC_SIZE, "\x7f" "ELF");
    bytes[CLASS_OFFSET] = CLASS_32_BIT;
    bytes[BYTE_ORDER_OFFSET] = LITTLE_ENDIAN_DATA;
    bytes[VERSION_OFFSET] = CURRENT_VERSION;

    put_u32(bytes, SECTION_TABLE_OFFSET, ELF_HEADER_SIZE);
    put_u16(bytes, HEADER_SIZE_OFFSET, ELF_HEADER_SIZE);
    put_u16(bytes, SECTION_ENTRY_SIZE_OFFSET, SECTION_ENTRY_SIZE);
    put_u16(bytes, SECTION_COUNT_OFFSET, static_cast<uint16_t>(count));

    for (size_t index = 0; index < count; index++) {
        size_t header = ELF_HEADER_SIZE + index * SECTION_ENTRY_SIZE;
        put_u32(bytes, header + SECTION_FLAGS_OFFSET, sections[index].flags);
        put_u32(bytes, header + SECTION_ADDRESS_OFFSET, sections[index].address);
        put_u32(bytes, header + SECTION_SIZE_OFFSET, sections[index].size);
    }

    return bytes;
}

static std::string write_temporary(const std::string& bytes) {
    char path[] = "/tmp/mcufw-elf-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || write(fd, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) {
        printf("  cannot write a temporary file\n");
        exit(1);
    }

    close(fd);
    return path;
}

static bool reads_ranges(const std::string& bytes, std::vector<address_range>& ranges, std::string& error) {
    std::string path = write_temporary(bytes);
    bool read = read_executable_ranges(path, ranges, error);
    unlink(path.c_str());

    return read;
}

int main() {
    std::vector<address_range> ranges;
    std::string error;
    bool read = reads_ranges(make_elf(ESP32_SECTIONS, std::size(ESP32_SECTIONS)), ranges, error);
    check(read, "reads an ESP32 build's sections");
    check(ranges.size() == 3, "keeps only the executable sections");
    if (ranges.size() == 3) {
        check(ranges[0].start == 0x40080000 && ranges[0].end == 0x40080400, "orders the vectors first");
        check(ranges[1].start == 0x40080400 && ranges[1].end == 0x40089400, "orders the IRAM code second");
        check(ranges[2].start == 0x400d0020 && ranges[2].end == 0x400f0020, "orders the flash code last");
    }

    std::string not_elf = make_elf(ESP32_SECTIONS, std::size(ESP32_SECTIONS));
    not_elf[MAGIC_SECOND_OFFSET] = 'X';
    check(!reads_ranges(not_elf, ranges, error) && error.find("is not an ELF file") != std::string::npos,
          "refuses a file that is no ELF file");

    std::string elf64 = make_elf(ESP32_SECTIONS, std::size(ESP32_SECTIONS));
    elf64[CLASS_OFFSET] = CLASS_64_BIT;
    check(!reads_ranges(elf64, ranges, error) && error.find("32-bit little-endian") != std::string::npos,
          "refuses a 64-bit ELF file");

    std::string big_endian = make_elf(ESP32_SECTIONS, std::size(ESP32_SECTIONS));
    big_endian[BYTE_ORDER_OFFSET] = BIG_ENDIAN_DATA;
    check(!reads_ranges(big_endian, ranges, error), "refuses a big-endian ELF file");

    std::string cut_short = make_elf(ESP32_SECTIONS, std::size(ESP32_SECTIONS));
    cut_short.resize(cut_short.size() - 1);
    check(!reads_ranges(cut_short, ranges, error) && error.find("section table past its end") != std::string::npos,
          "refuses a section table cut short");

    std::string short_entries = make_elf(ESP32_SECTIONS, std::size(ESP32_SECTIONS));
    put_u16(short_entries, SECTION_ENTRY_SIZE_OFFSET, SECTION_ENTRY_SIZE - 1);
    check(!reads_ranges(short_entries, ranges, error), "refuses section entries shorter than a section header");

    check(!reads_ranges("\x7f" "ELF", ranges, error), "refuses a file shorter than the ELF header");
    check(!read_executable_ranges("/tmp/mcufw-elf-missing", ranges, error), "fails for a missing file");

    if (g_failures != 0) {
        printf("elf: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("elf: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
