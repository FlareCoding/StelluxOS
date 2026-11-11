#include "quick_match.hpp"

#include <cstddef>

/* Shorter queries match too many names letter by letter to help */
constexpr size_t LETTERS_MIN = 3;

static char folded(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

static bool matches_at(std::string_view text, size_t at, std::string_view query) {
    if (text.size() - at < query.size()) {
        return false;
    }

    for (size_t i = 0; i < query.size(); i++) {
        if (folded(text[at + i]) != folded(query[i])) {
            return false;
        }
    }

    return true;
}

static bool starts_word(std::string_view text, size_t at) {
    char before = text[at - 1];
    return before == ' ' || before == '-' || before == '_' || before == '.' || before == '/';
}

static bool starts_keyword(std::string_view keywords, std::string_view query) {
    for (size_t at = 0; at < keywords.size(); at++) {
        bool is_word_start = at == 0 || keywords[at - 1] == ' ';
        if (is_word_start && keywords[at] != ' ' && matches_at(keywords, at, query)) {
            return true;
        }
    }

    return false;
}

/* The first place each letter of the query occurs after the one before, merged into runs, or none when a letter
 * never occurs */
static std::vector<match_run> letter_runs(std::string_view query, std::string_view name) {
    std::vector<match_run> runs;
    size_t at = 0;
    for (char letter : query) {
        while (at < name.size() && folded(name[at]) != folded(letter)) {
            at++;
        }

        if (at == name.size()) {
            return {};
        }

        bool extends_run = !runs.empty() && runs.back().start + runs.back().length == at;
        if (extends_run) {
            runs.back().length++;
        } else {
            runs.push_back({ static_cast<uint32_t>(at), 1 });
        }

        at++;
    }

    return runs;
}

query_match match_query(std::string_view query, std::string_view name, std::string_view keywords) {
    if (query.empty()) {
        return {};
    }

    uint32_t length = static_cast<uint32_t>(query.size());
    if (matches_at(name, 0, query)) {
        return { match_quality::prefix, { { 0, length } } };
    }

    for (size_t at = 1; at < name.size(); at++) {
        if (starts_word(name, at) && matches_at(name, at, query)) {
            return { match_quality::word, { { static_cast<uint32_t>(at), length } } };
        }
    }

    if (starts_keyword(keywords, query)) {
        return { match_quality::word, {} };
    }

    for (size_t at = 1; at < name.size(); at++) {
        if (matches_at(name, at, query)) {
            return { match_quality::substring, { { static_cast<uint32_t>(at), length } } };
        }
    }

    if (query.size() < LETTERS_MIN) {
        return {};
    }

    std::vector<match_run> runs = letter_runs(query, name);
    if (runs.empty()) {
        return {};
    }

    return { match_quality::letters, runs };
}
