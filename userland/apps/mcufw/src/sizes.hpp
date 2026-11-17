#ifndef MCUFW_SIZES_HPP
#define MCUFW_SIZES_HPP

// A built program's size checked as arduino-cli's `checkSize` checks it: the size recipe's stdout summed by the core's
// regexes, read in ECMAScript's dialect where arduino-cli uses RE2, with `^` matching at each line

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct program_size {
    uint64_t flash;
    uint64_t ram;
    bool     has_ram;  // the core states `recipe.size.regex.data`
};

struct size_verdict {
    std::string              failure;   // why the program does not fit the board, else empty
    std::vector<std::string> warnings;  // arduino-cli's own words, given when the program fits
};

// Fails when the core states no flash regex or a regex, the eeprom one included, does not compile
bool sum_program_size(std::string_view stdout_text, const std::string& flash_regex, const std::string& ram_regex,
                      const std::string& eeprom_regex, program_size& size, std::string& error);

// The board's limits read as Go's `strconv.Atoi` reads them, 0 for an unset, zero or negative one, which arduino-cli
// checks nothing against. Fails naming the key whose value is not a number.
bool read_size_limits(std::string_view maximum_size, std::string_view maximum_data_size, uint64_t& flash_max,
                      uint64_t& ram_max, std::string& error);

// Fails past a limit above 0, and past `warn_percentage` of the RAM limit, 75 when unset or invalid, warns
size_verdict check_program_size(const program_size& size, uint64_t flash_max, uint64_t ram_max,
                                std::string_view warn_percentage);

#endif // MCUFW_SIZES_HPP
