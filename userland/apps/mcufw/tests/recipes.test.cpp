/* recipes, run on the build machine: splitting a recipe line as Arduino's builder splits it, recipes that cannot
 * run, and the order of numbered recipes */

#include "recipes.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint32_t MAX_REPORTS = 8;

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> words;
    std::string error;
    check(split_recipe_line(line, words, error), "the line splits");
    return words;
}

static property_table parsed(const std::string& text) {
    property_table table;
    std::string error;
    check(parse_properties(text, table, error), "the text parses");
    return table;
}

static void check_splitting() {
    std::vector<std::string> words = split("\"/tools/bin/g++\" -c -DARDUINO_BOARD=\"ESP32_WROOM_DA\"  'a  b' x");
    std::vector<std::string> expected = { "/tools/bin/g++", "-c", "-DARDUINO_BOARD=\"ESP32_WROOM_DA\"", "a  b", "x" };
    check(words == expected, "quotes group words, a quote inside a word stays, and empty words are dropped");

    check(split("a\tb c") == std::vector<std::string>({ "a\tb", "c" }), "only spaces separate words");
    check(split("x \"\" y") == std::vector<std::string>({ "x", "y" }), "an empty quoted word is dropped");
    check(split("'it\"s' \"a 'b' c\"") == std::vector<std::string>({ "it\"s", "a 'b' c" }),
          "the other quote is plain text inside a quoted word");

    std::vector<std::string> unsplit;
    std::string error;
    check(!split_recipe_line("g++ \"-I/a b", unsplit, error), "a quote that does not close fails");
    check(error == "a \" quote does not close", "the failure names the quote");
}

static void check_recipe_words() {
    property_table properties = parsed(
        "compiler.path=/tools/bin/\n"
        "recipe.size.pattern=\"{compiler.path}size\" -A \"{build.path}/{build.project_name}.elf\"\n"
        "recipe.blank.pattern=   \n"
        "recipe.open.pattern=size \"-A\n");

    std::vector<std::string> words;
    std::string error;
    check(recipe_words(properties, "recipe.size.pattern", words, error), "a recipe expands and splits");
    check(words.size() == 3 && words[0] == "/tools/bin/size", "the program is the first word");
    check(words[2] == "{build.path}/{build.project_name}.elf", "unknown placeholders stay");

    check(!recipe_words(properties, "recipe.unset.pattern", words, error), "an unset recipe fails");
    check(error == "recipe.unset.pattern holds no command", "the failure names the recipe");
    check(!recipe_words(properties, "recipe.blank.pattern", words, error), "a recipe of spaces fails");
    check(!recipe_words(properties, "recipe.open.pattern", words, error), "a recipe whose quote does not close fails");
    check(error == "recipe.open.pattern does not split into words, since a \" quote does not close",
          "the failure says why");
}

static void check_recipe_keys() {
    property_table properties = parsed(
        "recipe.hooks.prebuild.pattern=never\n"
        "recipe.hooks.prebuild.2.pattern=two\n"
        "recipe.hooks.prebuild.10.pattern=ten\n"
        "recipe.hooks.prebuild.1.pattern=one\n"
        "recipe.hooks.prebuild.3.pattern=\n"
        "recipe.hooks.prebuild.4.label=four\n"
        "recipe.hooks.postbuild.pattern=alone\n"
        "recipe.hooks.objcopy.postobjcopy.pattern=\n"
        "recipe.hooks.linking.postlink.1.pattern=\n"
        "recipe.hooks.linking.postlink.pattern=bare\n");

    std::vector<std::string> numbered = recipe_keys(properties, "recipe.hooks.prebuild");
    std::vector<std::string> expected = {
        "recipe.hooks.prebuild.1.pattern",
        "recipe.hooks.prebuild.10.pattern",
        "recipe.hooks.prebuild.2.pattern",
    };

    check(numbered == expected, "numbered recipes holding a value run sorted as text, without the bare one");

    std::vector<std::string> bare = recipe_keys(properties, "recipe.hooks.postbuild");
    check(bare == std::vector<std::string>({ "recipe.hooks.postbuild.pattern" }), "a bare recipe runs alone");
    check(recipe_keys(properties, "recipe.hooks.objcopy.postobjcopy").empty(), "a bare recipe set empty is skipped");
    check(recipe_keys(properties, "recipe.hooks.linking.postlink") ==
              std::vector<std::string>({ "recipe.hooks.linking.postlink.pattern" }),
          "a bare recipe runs when every numbered one is empty");
    check(recipe_keys(properties, "recipe.hooks.linking.prelink").empty(), "no recipe means nothing runs");
}

int main() {
    check_splitting();
    check_recipe_words();
    check_recipe_keys();

    if (g_failures != 0) {
        printf("recipes: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("recipes: %llu checks passed\n", static_cast<unsigned long long>(g_checks));

    return 0;
}
