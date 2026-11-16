#include "versions.hpp"

#include <algorithm>
#include <string>
#include <vector>

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
