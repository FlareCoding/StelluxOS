#ifndef MCUFW_PROPERTIES_HPP
#define MCUFW_PROPERTIES_HPP

// Arduino's properties format as arduino-cli 1.6.0 reads it: the `key=value` lines of platform.txt, boards.txt and
// library.properties, and the `{key}` placeholders their recipes are written with

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Stellux's programs and serial ports follow Linux's conventions, so `cmd.linux=x` is read as `cmd=x`
constexpr std::string_view LINUX_KEY_PART = ".linux";

// arduino-cli stops expanding after this many passes, so a value naming itself cannot loop forever
constexpr int EXPANSION_PASSES_MAX = 10;

// Properties in the order arduino-cli keeps them. Setting a key moves it to the end, and that order decides the order
// of a board's menu choices.
class property_table {
public:
    void set(const std::string& key, const std::string& value);

    // The value of `key`, or nullptr when it is not set
    const std::string* find(const std::string& key) const;

    // The value of `key`, or an empty string when it is not set
    const std::string& get(const std::string& key) const;

    const std::vector<std::string>& keys() const;

    // Sets each key of `other` in its order, so its values replace these
    void merge(const property_table& other);

    // The keys that start with `prefix` and a dot, with both removed
    property_table subtree(std::string_view prefix) const;

    // The distinct parts of the keys before their first dot, in order of first appearance
    std::vector<std::string> first_level_keys() const;

    // Each part before a first dot with the subtree it heads, in order, leaving out keys that hold no dot
    std::vector<std::pair<std::string, property_table>> split_first_level() const;

    // Replaces each `{key}` whose key is set, innermost first, in passes until a pass changes nothing or
    // EXPANSION_PASSES_MAX passes ran. A placeholder naming no key stays as written.
    std::string expand(std::string_view text) const;

private:
    std::vector<std::string>                     m_keys;
    std::unordered_map<std::string, std::string> m_values;
};

// Reads properties text into `table` (blank and `#` lines skipped, keys and values trimmed, a key's first `.linux`
// dropped, text that is not UTF-8 read as ISO 8859-1). A line without `=` fails, the lines before it already set.
bool parse_properties(std::string_view text, property_table& table, std::string& error);

// The bytes of the UTF-8 character starting at `text[at]`, or 0 when none does by Unicode's definition, which refuses
// overlong forms and surrogates
size_t utf8_character_size(std::string_view text, size_t at);

// Reads the file at `path` as parse_properties does
bool load_properties(const std::string& path, property_table& table, std::string& error);

#endif // MCUFW_PROPERTIES_HPP
