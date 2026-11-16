#ifndef MCUFW_VERSIONS_HPP
#define MCUFW_VERSIONS_HPP

// Release versions as Arduino's indexes write them, compared as semantic versions: dot separated parts in turn, numbers
// by value and before text, a missing part as 0, a release after its prereleases, and build metadata after `+` ignored

#include <string_view>

// Below 0 when `a` is older than `b`, 0 when they name the same release, above 0 when `a` is newer
int compare_versions(std::string_view a, std::string_view b);

#endif // MCUFW_VERSIONS_HPP
