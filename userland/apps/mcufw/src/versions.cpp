#include "versions.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

constexpr std::string_view CONSTRAINT_SPACE = " \t\n\v\f\r";
constexpr size_t RELEASE_PARTS_MAX = 3;
constexpr std::string_view COMPARISONS[] = { "=", "^", ">", ">=", "<", "<=" };

static std::vector<std::string_view> split_parts(std::string_view text) {
    std::vector<std::string_view> parts;
    if (text.empty()) {
        return parts;
    }

    size_t start = 0;
    while (true) {
        size_t dot = text.find('.', start);
        if (dot == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }

        parts.push_back(text.substr(start, dot - start));
        start = dot + 1;
    }
}

static bool is_number(std::string_view part) {
    return !part.empty() && part.find_first_not_of("0123456789") == std::string_view::npos;
}

static int sign_of(int value) {
    return value < 0 ? -1 : (value > 0 ? 1 : 0);
}

// Numbers compare by value and before text, and text compares as text
static int compare_parts(std::string_view a, std::string_view b) {
    bool a_is_number = is_number(a);
    bool b_is_number = is_number(b);
    if (a_is_number != b_is_number) {
        return a_is_number ? -1 : 1;
    }

    if (!a_is_number) {
        return sign_of(a.compare(b));
    }

    a.remove_prefix(std::min(a.find_first_not_of('0'), a.size() - 1));
    b.remove_prefix(std::min(b.find_first_not_of('0'), b.size() - 1));
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }

    return sign_of(a.compare(b));
}

static int compare_releases(std::string_view a, std::string_view b) {
    std::vector<std::string_view> a_parts = split_parts(a);
    std::vector<std::string_view> b_parts = split_parts(b);
    size_t count = std::max(a_parts.size(), b_parts.size());
    for (size_t i = 0; i < count; i++) {
        std::string_view a_part = i < a_parts.size() ? a_parts[i] : "0";
        std::string_view b_part = i < b_parts.size() ? b_parts[i] : "0";
        int order = compare_parts(a_part, b_part);
        if (order != 0) {
            return order;
        }
    }

    return 0;
}

// A prerelease comes before its release, and among prereleases a shorter list of equal parts comes first
static int compare_prereleases(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) {
        return a.empty() == b.empty() ? 0 : (a.empty() ? 1 : -1);
    }

    std::vector<std::string_view> a_parts = split_parts(a);
    std::vector<std::string_view> b_parts = split_parts(b);
    size_t count = std::min(a_parts.size(), b_parts.size());
    for (size_t i = 0; i < count; i++) {
        int order = compare_parts(a_parts[i], b_parts[i]);
        if (order != 0) {
            return order;
        }
    }

    return a_parts.size() == b_parts.size() ? 0 : (a_parts.size() < b_parts.size() ? -1 : 1);
}

int compare_versions(std::string_view a, std::string_view b) {
    a = a.substr(0, a.find('+'));
    b = b.substr(0, b.find('+'));

    size_t a_dash = a.find('-');
    size_t b_dash = b.find('-');
    std::string_view a_prerelease = a_dash == std::string_view::npos ? std::string_view() : a.substr(a_dash + 1);
    std::string_view b_prerelease = b_dash == std::string_view::npos ? std::string_view() : b.substr(b_dash + 1);

    int order = compare_releases(a.substr(0, a_dash), b.substr(0, b_dash));
    if (order != 0) {
        return order;
    }

    return compare_prereleases(a_prerelease, b_prerelease);
}

// Go's TrimSpace for the ASCII spaces, which is all the index writes
static std::string_view trimmed(std::string_view text) {
    size_t first = text.find_first_not_of(CONSTRAINT_SPACE);
    if (first == std::string_view::npos) {
        return {};
    }

    return text.substr(first, text.find_last_not_of(CONSTRAINT_SPACE) - first + 1);
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_identifier_character(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

static bool is_version_character(char c) {
    return is_identifier_character(c) || c == '.' || c == '+';
}

// A number of a release part, which has no leading zero unless it is 0
static bool is_release_number(std::string_view part) {
    return is_number(part) && (part.size() == 1 || part[0] != '0');
}

// Identifiers separated by dots, as a prerelease or build metadata, a prerelease's numbers without leading zeros
static bool are_identifiers(std::string_view text, bool is_prerelease) {
    for (std::string_view identifier : split_parts(text)) {
        bool has_leading_zero = is_number(identifier) && identifier.size() > 1 && identifier[0] == '0';
        bool is_valid = !identifier.empty() &&
                        std::all_of(identifier.begin(), identifier.end(), is_identifier_character);
        if (!is_valid || (is_prerelease && has_leading_zero)) {
            return false;
        }
    }

    return !text.empty();
}

bool is_semantic_version(std::string_view text) {
    size_t plus = text.find('+');
    if (plus != std::string_view::npos && !are_identifiers(text.substr(plus + 1), false)) {
        return false;
    }

    std::string_view without_build = text.substr(0, plus);
    size_t dash = without_build.find('-');
    if (dash != std::string_view::npos && !are_identifiers(without_build.substr(dash + 1), true)) {
        return false;
    }

    std::vector<std::string_view> numbers = split_parts(without_build.substr(0, dash));

    return !numbers.empty() && numbers.size() <= RELEASE_PARTS_MAX &&
           std::all_of(numbers.begin(), numbers.end(), is_release_number);
}

static std::string_view release_part(const std::vector<std::string_view>& parts, size_t index) {
    return index < parts.size() ? parts[index] : std::string_view("0");
}

// relaxed-semver's `^`: no older than `bound`, with its major when that is not 0, else its minor when that is not 0,
// else its patch
static bool is_compatible(std::string_view bound, std::string_view version) {
    if (compare_versions(version, bound) < 0) {
        return false;
    }

    std::vector<std::string_view> bound_parts = split_parts(bound.substr(0, bound.find_first_of("-+")));
    std::vector<std::string_view> version_parts = split_parts(version.substr(0, version.find_first_of("-+")));
    for (size_t index = 0; index < RELEASE_PARTS_MAX; index++) {
        std::string_view bound_part = release_part(bound_parts, index);
        if (compare_parts(bound_part, release_part(version_parts, index)) != 0) {
            return false;
        }

        if (compare_parts(bound_part, "0") != 0) {
            return true;
        }
    }

    return true;
}

static bool holds(std::string_view comparison, std::string_view bound, std::string_view version) {
    int order = compare_versions(version, bound);
    if (comparison == "=") {
        return order == 0;
    }

    if (comparison == "^") {
        return is_compatible(bound, version);
    }

    if (comparison == ">" || comparison == ">=") {
        return order > 0 || (order == 0 && comparison == ">=");
    }

    return order < 0 || (order == 0 && comparison == "<=");
}

// A constraint read for one version, as relaxed-semver's ParseConstraint reads it
struct constraint_reader {
    std::string_view text;
    std::string_view version;
    size_t           position = 0;
};

static char peek(const constraint_reader& reader) {
    return reader.position < reader.text.size() ? reader.text[reader.position] : 0;
}

static char next(constraint_reader& reader) {
    char c = peek(reader);
    reader.position += c == 0 ? 0 : 1;

    return c;
}

static void skip_spaces(constraint_reader& reader) {
    while (peek(reader) == ' ') {
        reader.position++;
    }
}

static bool read_any(constraint_reader& reader, bool& meets);

// An operator and the version right after it, a negation, or a parenthesized constraint
static bool read_term(constraint_reader& reader, bool& meets) {
    skip_spaces(reader);
    char first = next(reader);
    if (first == '!') {
        bool negated = false;
        bool read = read_term(reader, negated);
        meets = !negated;

        return read;
    }

    if (first == '(') {
        bool read = read_any(reader, meets);
        skip_spaces(reader);

        return read && next(reader) == ')';
    }

    std::string comparison(1, first);
    if ((first == '>' || first == '<') && peek(reader) == '=') {
        comparison += next(reader);
    }

    size_t start = reader.position;
    while (is_version_character(peek(reader))) {
        reader.position++;
    }

    std::string_view bound = reader.text.substr(start, reader.position - start);
    bool is_comparison = std::find(std::begin(COMPARISONS), std::end(COMPARISONS), comparison) != std::end(COMPARISONS);
    if (!is_comparison || !is_semantic_version(bound)) {
        return false;
    }

    meets = holds(comparison, bound, reader.version);

    return true;
}

static bool read_all(constraint_reader& reader, bool& meets) {
    if (!read_term(reader, meets)) {
        return false;
    }

    while (true) {
        skip_spaces(reader);
        if (peek(reader) != '&') {
            return true;
        }

        bool term_meets = false;
        next(reader);
        if (next(reader) != '&' || !read_term(reader, term_meets)) {
            return false;
        }

        meets = meets && term_meets;
    }
}

// Ends before a closing parenthesis, which relaxed-semver also leaves unread at the top level
static bool read_any(constraint_reader& reader, bool& meets) {
    if (!read_all(reader, meets)) {
        return false;
    }

    while (true) {
        skip_spaces(reader);
        char c = peek(reader);
        if (c == 0 || c == ')') {
            return true;
        }

        bool alternative_meets = false;
        next(reader);
        if (c != '|' || next(reader) != '|' || !read_all(reader, alternative_meets)) {
            return false;
        }

        meets = meets || alternative_meets;
    }
}

bool check_constraint(std::string_view version, std::string_view constraint, bool& meets, std::string& error) {
    constraint_reader reader = { trimmed(constraint), version };
    meets = true;
    if (reader.text.empty()) {
        return true;
    }

    if (!read_any(reader, meets)) {
        error = "cannot read the version constraint \"" + std::string(constraint) + "\"";
        return false;
    }

    return true;
}
