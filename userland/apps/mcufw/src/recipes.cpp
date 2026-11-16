#include "recipes.hpp"

#include <algorithm>

constexpr std::string_view PATTERN_SUFFIX = ".pattern";
constexpr std::string_view WORD_SPACE = " \t\n\v\f\r";

static bool is_quote(char c) {
    return c == '"' || c == '\'';
}

static bool holds_more_than_space(std::string_view word) {
    return word.find_first_not_of(WORD_SPACE) != std::string_view::npos;
}

bool split_recipe_line(std::string_view line, std::vector<std::string>& words, std::string& error) {
    words.clear();
    char quote = 0;
    std::string quoted;
    size_t start = 0;
    while (start <= line.size()) {
        size_t end = line.find(' ', start);
        if (end == std::string_view::npos) {
            end = line.size();
        }

        std::string_view word = line.substr(start, end - start);
        start = end + 1;
        if (quote == 0 && (word.empty() || !is_quote(word.front()))) {
            if (holds_more_than_space(word)) {
                words.emplace_back(word);
            }

            continue;
        }

        if (quote == 0) {
            quote = word.front();
            word.remove_prefix(1);
            quoted.clear();
        }

        if (word.empty() || word.back() != quote) {
            quoted.append(word);
            quoted += ' ';
            continue;
        }

        quoted.append(word.substr(0, word.size() - 1));
        if (holds_more_than_space(quoted)) {
            words.push_back(quoted);
        }

        quote = 0;
    }

    if (quote != 0) {
        error = std::string("a ") + quote + " quote does not close";
        return false;
    }

    return true;
}

bool recipe_words(const property_table& properties, const std::string& key, std::vector<std::string>& words,
                  std::string& error) {
    if (!split_recipe_line(properties.expand(properties.get(key)), words, error)) {
        error = key + " does not split into words, since " + error;
        return false;
    }

    if (words.empty()) {
        error = key + " holds no command";
        return false;
    }

    return true;
}

std::vector<std::string> recipe_keys(const property_table& properties, std::string_view prefix) {
    std::string exact_key = std::string(prefix) + std::string(PATTERN_SUFFIX);
    std::vector<std::string> keys;
    for (const std::string& key : properties.keys()) {
        bool is_recipe = key.starts_with(prefix) && key.ends_with(PATTERN_SUFFIX);
        if (is_recipe && key != exact_key && !properties.get(key).empty()) {
            keys.push_back(key);
        }
    }

    if (keys.empty() && !properties.get(exact_key).empty()) {
        keys.push_back(exact_key);
    }

    std::sort(keys.begin(), keys.end());

    return keys;
}
