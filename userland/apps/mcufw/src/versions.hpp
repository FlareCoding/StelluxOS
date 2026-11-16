#ifndef MCUFW_VERSIONS_HPP
#define MCUFW_VERSIONS_HPP

// Release versions as Arduino's indexes write them, compared as semantic versions: dot separated parts in turn, numbers
// by value and before text, a missing part as 0, a release after its prereleases, and build metadata after `+` ignored

#include <string>
#include <string_view>

// Below 0 when `a` is older than `b`, 0 when they name the same release, above 0 when `a` is newer
int compare_versions(std::string_view a, std::string_view b);

// Whether relaxed-semver reads `text` as a version of one to three release numbers without leading zeros, then a
// prerelease after `-` and build metadata after `+`, each optional
bool is_semantic_version(std::string_view text);

// Whether `version` meets `constraint` in relaxed-semver's grammar, as arduino-cli 1.6.0 reads library dependencies:
// `=`, `>`, `>=`, `<`, `<=` and `^` before a version, `!`, `&&`, `||` and parentheses. Empty meets every version.
bool check_constraint(std::string_view version, std::string_view constraint, bool& meets, std::string& error);

#endif // MCUFW_VERSIONS_HPP
