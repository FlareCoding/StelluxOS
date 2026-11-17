#include "properties.hpp"

#include "files.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_set>

constexpr std::string_view LINE_SPACE = " \t\v\f";

static const std::string EMPTY_VALUE;

void property_table::set(const std::string& key, const std::string& value) {
    bool inserted = m_values.insert_or_assign(key, value).second;
    if (!inserted) {
        m_keys.erase(std::find(m_keys.begin(), m_keys.end(), key));
    }

    m_keys.push_back(key);
}

const std::string* property_table::find(const std::string& key) const {
    auto entry = m_values.find(key);
    return entry == m_values.end() ? nullptr : &entry->second;
}

const std::string& property_table::get(const std::string& key) const {
    const std::string* value = find(key);
    return value ? *value : EMPTY_VALUE;
}

const std::vector<std::string>& property_table::keys() const {
    return m_keys;
}

void property_table::merge(const property_table& other) {
    for (const std::string& key : other.m_keys) {
        set(key, other.m_values.at(key));
    }
}

property_table property_table::subtree(std::string_view prefix) const {
    std::string head = std::string(prefix) + ".";
    property_table tree;
    for (const std::string& key : m_keys) {
        if (key.compare(0, head.size(), head) == 0) {
            tree.set(key.substr(head.size()), m_values.at(key));
        }
    }

    return tree;
}

std::vector<std::string> property_table::first_level_keys() const {
    std::vector<std::string> parts;
    std::unordered_set<std::string> seen;
    for (const std::string& key : m_keys) {
        std::string part = key.substr(0, key.find('.'));
        if (seen.insert(part).second) {
            parts.push_back(part);
        }
    }

    return parts;
}

std::vector<std::pair<std::string, property_table>> property_table::split_first_level() const {
    std::vector<std::pair<std::string, property_table>> tables;
    std::unordered_map<std::string, size_t> index_of_part;
    for (const std::string& key : m_keys) {
        size_t dot = key.find('.');
        if (dot == std::string::npos) {
            continue;
        }

        std::string part = key.substr(0, dot);
        auto [entry, inserted] = index_of_part.try_emplace(part, tables.size());
        if (inserted) {
            tables.emplace_back(part, property_table());
        }

        tables[entry->second].second.set(key.substr(dot + 1), m_values.at(key));
    }

    return tables;
}

// One pass: each placeholder holding no other is replaced when its key is set
static std::string expand_innermost(const property_table& table, std::string_view text) {
    std::string expanded;
    size_t copied = 0;
    size_t open = std::string_view::npos;
    for (size_t at = 0; at < text.size(); at++) {
        if (text[at] == '{') {
            open = at;
            continue;
        }

        if (text[at] != '}' || open == std::string_view::npos) {
            continue;
        }

        const std::string* value = table.find(std::string(text.substr(open + 1, at - open - 1)));
        if (value) {
            expanded.append(text.substr(copied, open - copied));
            expanded += *value;
            copied = at + 1;
        }

        open = std::string_view::npos;
    }

    expanded.append(text.substr(copied));

    return expanded;
}

std::string property_table::expand(std::string_view text) const {
    std::string expanded(text);
    for (int pass = 0; pass < EXPANSION_PASSES_MAX; pass++) {
        std::string next = expand_innermost(*this, expanded);
        if (next == expanded) {
            break;
        }

        expanded = std::move(next);
    }

    return expanded;
}

static bool is_continuation(std::string_view text, size_t at) {
    return at < text.size() && (static_cast<uint8_t>(text[at]) & 0xC0) == 0x80;
}

size_t utf8_character_size(std::string_view text, size_t at) {
    uint8_t lead = static_cast<uint8_t>(text[at]);
    uint8_t second = at + 1 < text.size() ? static_cast<uint8_t>(text[at + 1]) : 0;
    size_t length = 0;
    bool second_in_range = true;
    if (lead < 0x80) {
        return 1;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        second_in_range = (lead != 0xE0 || second >= 0xA0) && (lead != 0xED || second <= 0x9F);
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        second_in_range = (lead != 0xF0 || second >= 0x90) && (lead != 0xF4 || second <= 0x8F);
    } else {
        return 0;
    }

    for (size_t i = 1; i < length; i++) {
        if (!is_continuation(text, at + i)) {
            return 0;
        }
    }

    return second_in_range ? length : 0;
}

static bool is_utf8(std::string_view text) {
    size_t at = 0;
    while (at < text.size()) {
        size_t length = utf8_character_size(text, at);
        if (length == 0) {
            return false;
        }

        at += length;
    }

    return true;
}

// Each byte of ISO 8859-1 text is the code point of the same number
static std::string latin1_to_utf8(std::string_view text) {
    std::string converted;
    for (char c : text) {
        uint8_t byte = static_cast<uint8_t>(c);
        if (byte < 0x80) {
            converted += c;
            continue;
        }

        converted += static_cast<char>(0xC0 | (byte >> 6));
        converted += static_cast<char>(0x80 | (byte & 0x3F));
    }

    return converted;
}

static std::string_view trimmed(std::string_view text) {
    size_t first = text.find_first_not_of(LINE_SPACE);
    if (first == std::string_view::npos) {
        return {};
    }

    size_t last = text.find_last_not_of(LINE_SPACE);

    return text.substr(first, last - first + 1);
}

static bool parse_line(std::string_view line, property_table& table) {
    std::string_view content = trimmed(line);
    if (content.empty() || content[0] == '#') {
        return true;
    }

    size_t equals = content.find('=');
    if (equals == std::string_view::npos) {
        return false;
    }

    std::string key(trimmed(content.substr(0, equals)));
    size_t linux_part = key.find(LINUX_KEY_PART);
    if (linux_part != std::string::npos) {
        key.erase(linux_part, LINUX_KEY_PART.size());
    }

    table.set(key, std::string(trimmed(content.substr(equals + 1))));

    return true;
}

bool parse_properties(std::string_view text, property_table& table, std::string& error) {
    std::string converted;
    if (!is_utf8(text)) {
        converted = latin1_to_utf8(text);
        text = converted;
    }

    size_t line_number = 1;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of("\r\n", start);
        if (end == std::string_view::npos) {
            end = text.size();
        }

        if (!parse_line(text.substr(start, end - start), table)) {
            error = "line " + std::to_string(line_number) + " has no equals sign";
            return false;
        }

        bool is_crlf = end + 1 < text.size() && text[end] == '\r' && text[end + 1] == '\n';
        start = end + (is_crlf ? 2 : 1);
        line_number++;
    }

    return true;
}

bool load_properties(const std::string& path, property_table& table, std::string& error) {
    std::string text;
    if (!read_file(path, text, error)) {
        return false;
    }

    if (!parse_properties(text, table, error)) {
        error = path + ", " + error;
        return false;
    }

    return true;
}

bool property_is_true(const property_table& properties, const std::string& key) {
    const std::string* value = properties.find(key);
    return value && trimmed(*value) == "true";
}
