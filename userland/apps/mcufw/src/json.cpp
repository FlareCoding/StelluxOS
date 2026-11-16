#include "json.hpp"

#include <cstdint>

enum class stream_position {
    before_object, before_key, key, after_key, before_value, value, after_value, before_element, element, after_element,
    done,
};

// Where the reader is in the document, and the key, value or element it is scanning
struct json_stream {
    std::string          array_key;
    json_element_handler take;
    stream_position      position = stream_position::before_object;
    bool                 first = true;  // the current object or array has no member or element yet
    std::string          key;           // the member whose value is read
    std::string          captured;      // the key, value or element being scanned, as written
    size_t               depth = 0;     // brackets and braces open in `captured`
    bool                 in_string = false;
    bool                 escaped = false;
    bool                 is_scalar = false;
};

constexpr std::string_view JSON_SPACE = " \t\n\r";
constexpr std::string_view ESCAPE_LETTERS = "\"\\/bfnrt";
constexpr std::string_view ESCAPED_BYTES = "\"\\/\b\f\n\r\t";
constexpr uint32_t SURROGATE_HIGH_FIRST = 0xd800;
constexpr uint32_t SURROGATE_LOW_FIRST = 0xdc00;
constexpr uint32_t SURROGATE_LOW_LAST = 0xdfff;

const json_value* json_member(const json_value& value, std::string_view name) {
    for (const auto& [member_name, member] : value.members) {
        if (member_name == name) {
            return &member;
        }
    }

    return nullptr;
}

static bool is_space(char c) {
    return JSON_SPACE.find(c) != std::string_view::npos;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static void skip_space(std::string_view text, size_t& position) {
    while (position < text.size() && is_space(text[position])) {
        position++;
    }
}

static int hex_digit_value(char c) {
    if (is_digit(c)) {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

static bool read_hex4(std::string_view text, size_t& position, uint32_t& unit) {
    if (position + 4 > text.size()) {
        return false;
    }

    unit = 0;
    for (size_t i = 0; i < 4; i++) {
        int digit = hex_digit_value(text[position++]);
        if (digit < 0) {
            return false;
        }

        unit = unit * 16 + static_cast<uint32_t>(digit);
    }

    return true;
}

static void append_utf8(std::string& out, uint32_t code_point) {
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xc0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3f));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xe0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code_point & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code_point & 0x3f));
    }
}

// A `\u` escape, joining a surrogate pair into one character
static bool read_unicode_escape(std::string_view text, size_t& position, std::string& out, std::string& error) {
    uint32_t unit = 0;
    if (!read_hex4(text, position, unit) || (unit >= SURROGATE_LOW_FIRST && unit <= SURROGATE_LOW_LAST)) {
        error = "a \\u escape names no character";
        return false;
    }

    if (unit >= SURROGATE_HIGH_FIRST && unit < SURROGATE_LOW_FIRST) {
        uint32_t low = 0;
        bool has_low = text.substr(position, 2) == "\\u";
        position += 2;
        if (!has_low || !read_hex4(text, position, low) || low < SURROGATE_LOW_FIRST || low > SURROGATE_LOW_LAST) {
            error = "a \\u escape names no character";
            return false;
        }

        unit = 0x10000 + ((unit - SURROGATE_HIGH_FIRST) << 10) + (low - SURROGATE_LOW_FIRST);
    }

    append_utf8(out, unit);

    return true;
}

static bool parse_string(std::string_view text, size_t& position, std::string& out, std::string& error) {
    position++;
    while (position < text.size()) {
        char c = text[position++];
        if (c == '"') {
            return true;
        }

        if (static_cast<unsigned char>(c) < 0x20) {
            error = "a string holds a control character";
            return false;
        }

        if (c != '\\') {
            out += c;
            continue;
        }

        char letter = position < text.size() ? text[position++] : 'x';
        size_t index = ESCAPE_LETTERS.find(letter);
        if (letter == 'u') {
            if (!read_unicode_escape(text, position, out, error)) {
                return false;
            }
        } else if (index != std::string_view::npos) {
            out += ESCAPED_BYTES[index];
        } else {
            error = "a string holds an unknown escape";
            return false;
        }
    }

    error = "a string does not end";

    return false;
}

static bool skip_digits(std::string_view text, size_t& position) {
    size_t start = position;
    while (position < text.size() && is_digit(text[position])) {
        position++;
    }

    return position > start;
}

// RFC 8259's number: an optional minus, an integer without leading zeros, then an optional fraction and exponent
static bool parse_number(std::string_view text, size_t& position, std::string& out, std::string& error) {
    size_t start = position;
    if (text[position] == '-') {
        position++;
    }

    bool is_valid = true;
    if (position < text.size() && text[position] == '0') {
        position++;
    } else {
        is_valid = skip_digits(text, position);
    }

    if (is_valid && position < text.size() && text[position] == '.') {
        position++;
        is_valid = skip_digits(text, position);
    }

    if (is_valid && position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        position++;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            position++;
        }

        is_valid = skip_digits(text, position);
    }

    if (!is_valid) {
        error = "a number is malformed";
        return false;
    }

    out = text.substr(start, position - start);

    return true;
}

// Each value parsed counts in `values`, so an element's parsed size stays bounded
struct parse_budget {
    size_t values = 0;
};

static bool parse_value(std::string_view text, size_t& position, size_t depth, parse_budget& budget, json_value& value,
                        std::string& error);

static bool parse_member_name(std::string_view text, size_t& position, std::string& name, std::string& error) {
    skip_space(text, position);
    if (position >= text.size() || text[position] != '"') {
        error = "an object's member has no name";
        return false;
    }

    if (!parse_string(text, position, name, error)) {
        return false;
    }

    skip_space(text, position);
    if (position >= text.size() || text[position] != ':') {
        error = "a member's name has no `:` after it";
        return false;
    }

    position++;

    return true;
}

static bool parse_container(std::string_view text, size_t& position, size_t depth, parse_budget& budget,
                            json_value& value, std::string& error) {
    if (depth >= JSON_DEPTH_MAX) {
        error = "values nest deeper than " + std::to_string(JSON_DEPTH_MAX);
        return false;
    }

    bool is_object = text[position] == '{';
    char close = is_object ? '}' : ']';
    value.kind = is_object ? json_kind::object : json_kind::array;
    position++;
    skip_space(text, position);
    if (position < text.size() && text[position] == close) {
        position++;
        return true;
    }

    while (true) {
        std::string name;
        if (is_object && !parse_member_name(text, position, name, error)) {
            return false;
        }

        json_value item;
        if (!parse_value(text, position, depth + 1, budget, item, error)) {
            return false;
        }

        if (is_object) {
            value.members.emplace_back(std::move(name), std::move(item));
        } else {
            value.items.push_back(std::move(item));
        }

        skip_space(text, position);
        char next = position < text.size() ? text[position++] : 0;
        if (next == close) {
            return true;
        }

        if (next != ',') {
            error = std::string("a `,` or `") + close + "` is missing";
            return false;
        }
    }
}

static bool parse_literal(std::string_view text, size_t& position, json_value& value, std::string& error) {
    for (std::string_view literal : { "true", "false", "null" }) {
        if (text.substr(position, literal.size()) == literal) {
            position += literal.size();
            value.kind = literal == "null" ? json_kind::null : json_kind::boolean;
            value.text = literal == "null" ? "" : std::string(literal);
            return true;
        }
    }

    error = "a value is malformed";

    return false;
}

static bool parse_value(std::string_view text, size_t& position, size_t depth, parse_budget& budget, json_value& value,
                        std::string& error) {
    skip_space(text, position);
    if (position >= text.size()) {
        error = "a value is missing";
        return false;
    }

    if (++budget.values > JSON_VALUES_MAX) {
        error = "an element holds more than " + std::to_string(JSON_VALUES_MAX) + " values";
        return false;
    }

    char c = text[position];
    if (c == '{' || c == '[') {
        return parse_container(text, position, depth, budget, value, error);
    }

    if (c == '"') {
        value.kind = json_kind::string;
        return parse_string(text, position, value.text, error);
    }

    if (c == '-' || is_digit(c)) {
        value.kind = json_kind::number;
        return parse_number(text, position, value.text, error);
    }

    return parse_literal(text, position, value, error);
}

// `text` as one whole value with nothing but space around it
static bool parse_document(std::string_view text, json_value& value, std::string& error) {
    size_t position = 0;
    parse_budget budget;
    if (!parse_value(text, position, 0, budget, value, error)) {
        return false;
    }

    skip_space(text, position);
    if (position != text.size()) {
        error = "a value has text after it";
        return false;
    }

    return true;
}

static void start_scan(json_stream& state, stream_position position) {
    state.position = position;
    state.captured.clear();
    state.depth = 0;
    state.in_string = false;
    state.escaped = false;
    state.is_scalar = false;
}

// Adds `c` to the value being scanned. A number or literal ends at the byte after it, which `consumed` leaves unread.
static bool scan(json_stream& state, char c, bool& complete, bool& consumed, std::string& error) {
    complete = false;
    consumed = true;
    if (state.captured.empty()) {
        state.in_string = c == '"';
        state.is_scalar = c != '"' && c != '{' && c != '[';
        state.depth = state.in_string || state.is_scalar ? 0 : 1;
        state.captured += c;
        return true;
    }

    if (state.is_scalar && (is_space(c) || c == ',' || c == '}' || c == ']')) {
        complete = true;
        consumed = false;
        return true;
    }

    if (state.captured.size() >= JSON_ELEMENT_MAX) {
        error = "a value is longer than " + std::to_string(JSON_ELEMENT_MAX >> 20) + " MiB";
        return false;
    }

    state.captured += c;
    if (state.in_string) {
        bool closes = !state.escaped && c == '"';
        state.escaped = !state.escaped && c == '\\';
        state.in_string = !closes;
        complete = closes && state.depth == 0;
    } else if (c == '"') {
        state.in_string = true;
    } else if ((c == '{' || c == '[') && !state.is_scalar) {
        state.depth++;
    } else if ((c == '}' || c == ']') && !state.is_scalar) {
        state.depth--;
        complete = state.depth == 0;
    }

    return true;
}

// The key, value or element just scanned, handed to `take` when it is an element of the array
static bool finish_scan(json_stream& state, std::string& error) {
    json_value value;
    if (!parse_document(state.captured, value, error)) {
        return false;
    }

    if (state.position == stream_position::key) {
        state.key = value.text;
        state.position = stream_position::after_key;
        return true;
    }

    if (state.position == stream_position::value) {
        state.position = stream_position::after_value;
        return true;
    }

    state.position = stream_position::after_element;

    return state.take(value, error);
}

// A byte between tokens, starting a scan with `consumed` false when it opens a key, value or element
static bool step_between(json_stream& state, char c, bool& consumed, std::string& error) {
    stream_position at = state.position;
    if (at == stream_position::before_object && c == '{') {
        state.position = stream_position::before_key;
        state.first = true;
        return true;
    }

    if ((at == stream_position::before_key && state.first && c == '}') ||
        (at == stream_position::after_value && c == '}')) {
        state.position = stream_position::done;
        return true;
    }

    if (at == stream_position::before_key && c == '"') {
        start_scan(state, stream_position::key);
        consumed = false;
        return true;
    }

    if (at == stream_position::after_key && c == ':') {
        state.position = stream_position::before_value;
        return true;
    }

    if (at == stream_position::before_value && state.key == state.array_key) {
        if (c != '[') {
            error = "`" + state.array_key + "` is no array";
            return false;
        }

        state.position = stream_position::before_element;
        state.first = true;
        return true;
    }

    if (at == stream_position::before_value || (at == stream_position::before_element && c != ']')) {
        start_scan(state, at == stream_position::before_value ? stream_position::value : stream_position::element);
        consumed = false;
        return true;
    }

    if ((at == stream_position::before_element && state.first && c == ']') ||
        (at == stream_position::after_element && c == ']')) {
        state.position = stream_position::after_value;
        return true;
    }

    if (at == stream_position::after_element && c == ',') {
        state.position = stream_position::before_element;
        state.first = false;
        return true;
    }

    if (at == stream_position::after_value && c == ',') {
        state.position = stream_position::before_key;
        state.first = false;
        return true;
    }

    error = at == stream_position::done ? "the document has text after it" : "the document is malformed";

    return false;
}

// One byte, which `consumed` leaves unread when it belongs to the next token
static bool step(json_stream& state, char c, bool& consumed, std::string& error) {
    consumed = true;
    bool is_scanning = state.position == stream_position::key || state.position == stream_position::value ||
                       state.position == stream_position::element;
    if (!is_scanning) {
        return is_space(c) || step_between(state, c, consumed, error);
    }

    bool complete = false;
    if (!scan(state, c, complete, consumed, error)) {
        return false;
    }

    return !complete || finish_scan(state, error);
}

json_array_reader::json_array_reader(std::string array_key, json_element_handler take)
    : m_stream(std::make_unique<json_stream>()) {
    m_stream->array_key = std::move(array_key);
    m_stream->take = std::move(take);
}

json_array_reader::~json_array_reader() = default;

bool json_array_reader::feed(std::string_view bytes, std::string& error) {
    size_t index = 0;
    while (index < bytes.size()) {
        bool consumed = true;
        if (!step(*m_stream, bytes[index], consumed, error)) {
            return false;
        }

        index += consumed ? 1 : 0;
    }

    return true;
}

bool json_array_reader::finish(std::string& error) {
    if (m_stream->position != stream_position::done) {
        error = "the document ends early";
        return false;
    }

    return true;
}
