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
    size_t ignored = 0;
    return recipe_words(properties, key, words, ignored, error);
}

bool recipe_words(const property_table& properties, const std::string& key, std::vector<std::string>& words,
                  size_t& text_length, std::string& error) {
    std::string text = properties.expand(properties.get(key));
    text_length = text.size();
    if (!split_recipe_line(text, words, error)) {
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

// A placeholder is `{`, at least one character other than a newline, and the first `}` after them
std::string drop_unknown_placeholders(std::string_view text) {
    std::string kept;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            size_t close = text.find('}', i + 2);
            bool on_one_line = close != std::string_view::npos &&
                               text.substr(i, close - i).find('\n') == std::string_view::npos;
            if (on_one_line) {
                i = close + 1;
                continue;
            }
        }

        kept += text[i];
        i++;
    }

    return kept;
}

static void replace_first(std::string& text, std::string_view placeholder, std::string_view replacement) {
    size_t at = text.find(placeholder);
    if (at != std::string::npos) {
        text.replace(at, placeholder.size(), replacement);
    }
}

bool preprocess_words(const property_table& properties, const std::string& source, const std::string& output,
                      const std::vector<std::string>& include_directories, const std::string& depfile,
                      std::vector<std::string>& words, std::string& error) {
    property_table preprocessing;
    preprocessing.set("preproc.macros.flags", "-w -x c++ -E -CC");
    preprocessing.merge(properties);
    preprocessing.set("build.library_discovery_phase", "1");
    preprocessing.set("source_file", source);
    preprocessing.set("preprocessed_file_path", output);

    std::string includes;
    for (const std::string& directory : include_directories) {
        includes += (includes.empty() ? "\"-I" : " \"-I") + directory + "\"";
    }

    preprocessing.set("includes", includes);

    std::string pattern = preprocessing.get("recipe.preproc.macros");
    if (pattern.empty()) {
        pattern = preprocessing.get("recipe.cpp.o.pattern");
        replace_first(pattern, "{compiler.cpp.flags}", "{compiler.cpp.flags} {preproc.macros.flags}");
        replace_first(pattern, "{object_file}", "{preprocessed_file_path}");
    }

    if (!split_recipe_line(drop_unknown_placeholders(preprocessing.expand(pattern)), words, error)) {
        error = "the preprocessor's recipe cannot split, since " + error;
        return false;
    }

    std::erase(words, "-MMD");
    if (words.empty()) {
        error = "the board's preprocessor recipe holds no words";
        return false;
    }

    if (!depfile.empty()) {
        words.insert(words.end(), { "-MMD", "-MF", depfile });
    }

    return true;
}
