#ifndef MCUFW_ELF_HPP
#define MCUFW_ELF_HPP

// The address ranges of a 32-bit little-endian ELF file's executable sections, the format the compilers of every
// architecture Arduino's cores target write

#include <cstdint>
#include <string>
#include <vector>

struct address_range {
    uint32_t start;
    uint32_t end;  // past the last byte
};

// The ranges of the sections flagged executable, ordered by start address
bool read_executable_ranges(const std::string& path, std::vector<address_range>& ranges, std::string& error);

#endif // MCUFW_ELF_HPP
