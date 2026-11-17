#ifndef MCUFW_RECIPES_HPP
#define MCUFW_RECIPES_HPP

// Recipes turned into commands as arduino-cli 1.6.0 turns them

#include "properties.hpp"

#include <string>
#include <string_view>
#include <vector>

// Splits at single spaces, with no escapes. A word opening with `"` or `'` runs to the word closing with that quote,
// both dropped, so the quotes inside `-DBOARD="DA"` stay. Blank words are dropped, and an unclosed quote fails.
bool split_recipe_line(std::string_view line, std::vector<std::string>& words, std::string& error);

// The recipe `key` expanded and split, with `text_length` the expanded text's length, which arduino-cli's 30,000
// character rule measures. Fails when the recipe is unset or holds no words, or a quote does not close.
bool recipe_words(const property_table& properties, const std::string& key, std::vector<std::string>& words,
                  size_t& text_length, std::string& error);
bool recipe_words(const property_table& properties, const std::string& key, std::vector<std::string>& words,
                  std::string& error);

// The keys starting with `prefix` and ending in `.pattern` that hold a value, sorted as text, else `<prefix>.pattern`
// when it holds a value, as arduino-cli 1.6.0 finds recipes. A hook point's prefix is `recipe.hooks.<point>`.
std::vector<std::string> recipe_keys(const property_table& properties, std::string_view prefix);

// `text` with every `{...}` left in it removed, as Arduino's builder removes them before a preprocessor run
std::string drop_unknown_placeholders(std::string_view text);

// The board's preprocessor on `source` into `output` as arduino-cli's `preprocessor.GCC` builds it, from the core's
// preprocessor recipe or its C++ recipe, `-MMD` dropped and `-MMD -MF <depfile>` added for a `depfile`
bool preprocess_words(const property_table& properties, const std::string& source, const std::string& output,
                      const std::vector<std::string>& include_directories, const std::string& depfile,
                      std::vector<std::string>& words, std::string& error);

#endif // MCUFW_RECIPES_HPP
