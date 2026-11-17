#ifndef MCUFW_LIBRARY_HINT_HPP
#define MCUFW_LIBRARY_HINT_HPP

// The library to name when no installed library provides a header: among the newest releases in Arduino's library
// index whose `providesIncludes` lists it, the one ranked first as installed libraries are, ties to the first name

#include <string>

struct library_suggestion {
    std::string name;
    std::string version;
};

// Fails when the index is missing or unreadable, or no newest release lists the header
bool suggest_library(const std::string& library_index, const std::string& header, const std::string& architecture,
                     library_suggestion& suggestion, std::string& error);

#endif // MCUFW_LIBRARY_HINT_HPP
