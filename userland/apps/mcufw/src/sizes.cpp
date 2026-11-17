#include "sizes.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <regex>

// arduino-cli warns past this share of the board's RAM unless the board states its own
constexpr int64_t DATA_WARNING_PERCENT = 75;
constexpr int64_t PERCENT = 100;
constexpr int64_t DECIMAL_BASE = 10;

// A number as Go's `strconv.Atoi` reads one: an optional sign, then decimal digits, within a signed 64-bit range
static bool read_signed_decimal(std::string_view text, int64_t& value) {
    bool negative = !text.empty() && text[0] == '-';
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
        text.remove_prefix(1);
    }

    if (text.empty()) {
        return false;
    }

    uint64_t limit = negative ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1
                              : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t magnitude = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }

        uint64_t digit = static_cast<uint64_t>(c - '0');
        if (magnitude > (limit - digit) / DECIMAL_BASE) {
            return false;
        }

        magnitude = magnitude * DECIMAL_BASE + digit;
    }

    value = negative ? static_cast<int64_t>(0 - magnitude) : static_cast<int64_t>(magnitude);

    return true;
}

static bool compile_regex(const std::string& key, const std::string& pattern, std::regex& expression,
                          std::string& error) {
    try {
        expression = std::regex(pattern, std::regex::ECMAScript | std::regex::multiline);
    } catch (const std::regex_error& failure) {
        error = key + " " + pattern + " does not compile: " + failure.what();
        return false;
    }

    return true;
}

// Adds each match of `pattern`, and each group of a match, that reads as a whole number
static bool sum_matches(std::string_view text, const std::string& key, const std::string& pattern, uint64_t& total,
                        std::string& error) {
    std::regex expression;
    if (!compile_regex(key, pattern, expression, error)) {
        return false;
    }

    int64_t sum = 0;
    using text_iterator = std::regex_iterator<std::string_view::const_iterator>;
    for (text_iterator match(text.begin(), text.end(), expression), end; match != end; ++match) {
        for (size_t part = 0; part < match->size(); part++) {
            int64_t count = 0;
            if (read_signed_decimal((*match)[part].str(), count)) {
                sum += count;
            }
        }
    }

    total = static_cast<uint64_t>(std::max<int64_t>(sum, 0));

    return true;
}

bool sum_program_size(std::string_view stdout_text, const std::string& flash_regex, const std::string& ram_regex,
                      const std::string& eeprom_regex, program_size& size, std::string& error) {
    if (flash_regex.empty()) {
        error = "the core sets no recipe.size.regex";
        return false;
    }

    size = {};
    std::regex eeprom;
    if (!sum_matches(stdout_text, "recipe.size.regex", flash_regex, size.flash, error) ||
        (!eeprom_regex.empty() && !compile_regex("recipe.size.regex.eeprom", eeprom_regex, eeprom, error))) {
        return false;
    }

    size.has_ram = !ram_regex.empty();

    return !size.has_ram || sum_matches(stdout_text, "recipe.size.regex.data", ram_regex, size.ram, error);
}

static bool read_limit(std::string_view key, std::string_view text, uint64_t& limit, std::string& error) {
    int64_t value = 0;
    if (!text.empty() && !read_signed_decimal(text, value)) {
        error = "the board's " + std::string(key) + ", " + std::string(text) + ", is not a number";
        return false;
    }

    limit = static_cast<uint64_t>(std::max<int64_t>(value, 0));

    return true;
}

bool read_size_limits(std::string_view maximum_size, std::string_view maximum_data_size, uint64_t& flash_max,
                      uint64_t& ram_max, std::string& error) {
    return read_limit("upload.maximum_size", maximum_size, flash_max, error) &&
           read_limit("upload.maximum_data_size", maximum_data_size, ram_max, error);
}

size_verdict check_program_size(const program_size& size, uint64_t flash_max, uint64_t ram_max,
                                std::string_view warn_percentage) {
    size_verdict verdict;
    if (flash_max > 0 && size.flash > flash_max) {
        verdict.failure = "the sketch takes " + std::to_string(size.flash) + " bytes of flash, more than the board's " +
                          std::to_string(flash_max);
        return verdict;
    }

    if (ram_max > 0 && size.has_ram && size.ram > ram_max) {
        verdict.failure = "the sketch's globals take " + std::to_string(size.ram) +
                          " bytes of RAM, more than the board's " + std::to_string(ram_max);
        return verdict;
    }

    int64_t percent = DATA_WARNING_PERCENT;
    if (!warn_percentage.empty() && !read_signed_decimal(warn_percentage, percent)) {
        verdict.warnings.push_back("Invalid value for build.warn_data_percentage: " + std::string(warn_percentage));
        percent = DATA_WARNING_PERCENT;
    }

    int64_t threshold = static_cast<int64_t>(ram_max) * percent / PERCENT;
    if (ram_max > 0 && size.has_ram && static_cast<int64_t>(size.ram) > threshold) {
        verdict.warnings.push_back("Low memory available, stability problems may occur.");
    }

    return verdict;
}
