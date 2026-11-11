/* parse_chord and chord_keycaps, run on the build machine: every key name reaches its HID usage and keycap, and a
 * chord that would take a typing key from every app is refused */

#include "shell/chord.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct parsed_chord {
    const char* chord;
    uint8_t mods;
    uint16_t usage;
};

struct chord_caps {
    const char* chord;
    const char* keycaps;
};

constexpr uint32_t MAX_REPORTS = 8;
constexpr uint16_t HID_USAGE_F1 = 0x3A;
constexpr uint32_t FUNCTION_KEYS = 12;

static const parsed_chord PARSED[] = {
    { "ctrl+alt+t", CHORD_MOD_CTRL | CHORD_MOD_ALT, 0x17 },
    { "super+s", CHORD_MOD_SUPER, 0x16 },
    { "super+shift+3", CHORD_MOD_SUPER | CHORD_MOD_SHIFT, 0x20 },
    { "gui+0", CHORD_MOD_SUPER, 0x27 },
    { "ctrl+enter", CHORD_MOD_CTRL, 0x28 },
    { "alt+escape", CHORD_MOD_ALT, 0x29 },
    { "alt+tab", CHORD_MOD_ALT, 0x2B },
    { "super+space", CHORD_MOD_SUPER, 0x2C },
    { "f1", 0, 0x3A },
    { "super+f5", CHORD_MOD_SUPER, 0x3E },
    { "shift+f12", CHORD_MOD_SHIFT, 0x45 },
    { "print", 0, 0x46 },
    { "shift+print", CHORD_MOD_SHIFT, 0x46 },
    { "alt+print", CHORD_MOD_ALT, 0x46 },
    { "super+right", CHORD_MOD_SUPER, 0x4F },
    { "super+left", CHORD_MOD_SUPER, 0x50 },
    { "super+down", CHORD_MOD_SUPER, 0x51 },
    { "super+up", CHORD_MOD_SUPER, 0x52 },
};

static const char* const REFUSED[] = {
    "", "super", "ctrl+shift", "ctrl++t", "a", "shift+a", "7", "space", "shift+space", "enter", "escape", "tab", "up",
    "super+f0", "super+f13", "super+fn", "super+Space", "super+spacebar", "super+printscreen",
};

static const chord_caps KEYCAPS[] = {
    { "ctrl+alt+t", "Ctrl Alt T" },
    { "super+space", "Super Space" },
    { "shift+print", "Shift Print" },
    { "super+f12", "Super F12" },
    { "gui+escape", "Super Escape" },
    { "space", "" },
};

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what, const char* chord) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s: \"%s\"\n", what, chord);
    }
}

static std::string joined(const std::vector<std::string>& keys) {
    std::string out;
    for (const std::string& key : keys) {
        out += out.empty() ? key : " " + key;
    }

    return out;
}

int main() {
    for (const parsed_chord& expected : PARSED) {
        uint8_t mods = 0xFF;
        uint16_t usage = 0;
        bool parsed = parse_chord(expected.chord, &mods, &usage);
        check(parsed && mods == expected.mods && usage == expected.usage, "parses to its modifiers and usage",
              expected.chord);
    }

    for (uint32_t n = 1; n <= FUNCTION_KEYS; n++) {
        char chord[16];
        snprintf(chord, sizeof(chord), "super+f%u", static_cast<unsigned>(n));
        uint8_t mods = 0;
        uint16_t usage = 0;
        bool parsed = parse_chord(chord, &mods, &usage);
        check(parsed && usage == HID_USAGE_F1 + n - 1, "names its function key", chord);
    }

    for (const char* chord : REFUSED) {
        uint8_t mods = 0;
        uint16_t usage = 0;
        check(!parse_chord(chord, &mods, &usage), "is refused", chord);
    }

    for (const chord_caps& expected : KEYCAPS) {
        check(joined(chord_keycaps(expected.chord)) == expected.keycaps, "shows its keycaps", expected.chord);
    }

    if (g_failures != 0) {
        printf("chord: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("chord: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
