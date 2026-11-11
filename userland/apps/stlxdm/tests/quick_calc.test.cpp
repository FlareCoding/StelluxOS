/* calculate, copied_number and shown_number, run on the build machine: precedence and signs as written math reads
 * them, the inputs that are not calculations, and numbers as the palette shows and copies them */

#include "shell/quick_calc.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

struct calculation {
    const char* text;
    double      value;
};

struct number_forms {
    double      value;
    const char* copied;
    const char* shown;
};

constexpr uint32_t MAX_REPORTS = 8;

static const calculation CALCULATIONS[] = {
    { "1920 * 1080 * 4", 8294400 },
    { "2 + 3 * 4", 14 },
    { "(2 + 3) * 4", 20 },
    { "2 ^ 10", 1024 },
    { "2^3^2", 512 },
    { "-2^2", -4 },
    { "2^-1", 0.5 },
    { "10 % 3", 1 },
    { "7 / 2", 3.5 },
    { "1.5 + .5", 2 },
    { "5. * 2", 10 },
    { "3 * -2", -6 },
    { "2 - -3", 5 },
    { "  1+1 ", 2 },
    { "-(4 - 6) * 2", 4 },
};

static const char* const NOT_CALCULATIONS[] = {
    "", "42", "-5", "(42)", "+7", "1 / 0", "5 % 0", "2 +", "(1 + 2", "1 + 2)", "abc", "1e5 + 1", "2(3)", ". + 1",
    "1 ++", "vim /etc/stlxdm/stlxdm.conf",
};

static const number_forms FORMS[] = {
    { 8294400, "8294400", "8,294,400" },
    { -1234.5, "-1234.5", "-1,234.5" },
    { 1.0 / 3, "0.3333333333", "0.3333333333" },
    { 999, "999", "999" },
    { 1000, "1000", "1,000" },
    { 123456789012, "123456789012", "123,456,789,012" },
    { 1e20, "1e+20", "1e+20" },
    { -0.0, "0", "0" },
    { 0.1 + 0.2, "0.3", "0.3" },
};

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what, const std::string& subject) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s: \"%s\"\n", what, subject.c_str());
    }
}

int main() {
    for (const calculation& expected : CALCULATIONS) {
        double value = 0;
        bool calculated = calculate(expected.text, &value);
        check(calculated && value == expected.value, "calculates its value", expected.text);
    }

    for (const char* text : NOT_CALCULATIONS) {
        double value = 0;
        check(!calculate(text, &value), "is not a calculation", text);
    }

    for (const number_forms& expected : FORMS) {
        check(copied_number(expected.value) == expected.copied, "copies as", expected.copied);
        check(shown_number(expected.value) == expected.shown, "shows as", expected.shown);
    }

    if (g_failures != 0) {
        printf("quick_calc: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("quick_calc: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
