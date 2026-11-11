#include "chord.hpp"

#include <cctype>
#include <cstring>

/* A key a chord names with a word, and its usage on the HID keyboard page */
struct named_key {
    const char* name;
    uint16_t usage;
};

constexpr uint16_t HID_USAGE_A = 0x04;
constexpr uint16_t HID_USAGE_1 = 0x1E;
constexpr uint16_t HID_USAGE_0 = 0x27;
constexpr uint16_t HID_USAGE_F1 = 0x3A;
constexpr uint16_t HID_USAGE_PRINT = 0x46;

/* The modifiers that turn a typing key into a command */
constexpr uint8_t COMMAND_MODS = CHORD_MOD_CTRL | CHORD_MOD_ALT | CHORD_MOD_SUPER;

constexpr named_key NAMED_KEYS[] = {
    { "enter", 0x28 },
    { "escape", 0x29 },
    { "tab", 0x2B },
    { "space", 0x2C },
    { "f1", HID_USAGE_F1 },
    { "f2", 0x3B },
    { "f3", 0x3C },
    { "f4", 0x3D },
    { "f5", 0x3E },
    { "f6", 0x3F },
    { "f7", 0x40 },
    { "f8", 0x41 },
    { "f9", 0x42 },
    { "f10", 0x43 },
    { "f11", 0x44 },
    { "f12", 0x45 },
    { "print", HID_USAGE_PRINT },
    { "right", 0x4F },
    { "left", 0x50 },
    { "down", 0x51 },
    { "up", 0x52 },
};

/* Gives the length of the key at `key` and returns where the chord's next key starts */
static const char* next_chord_key(const char* key, size_t* length) {
    const char* end = strchr(key, '+');
    *length = end ? static_cast<size_t>(end - key) : strlen(key);

    return end ? end + 1 : key + *length;
}

/* The usage of a chord's key, 0 when no key has that name */
static uint16_t key_usage(const char* key, size_t length) {
    if (length == 1 && *key >= 'a' && *key <= 'z') {
        return static_cast<uint16_t>(HID_USAGE_A + (*key - 'a'));
    }

    if (length == 1 && *key >= '1' && *key <= '9') {
        return static_cast<uint16_t>(HID_USAGE_1 + (*key - '1'));
    }

    if (length == 1 && *key == '0') {
        return HID_USAGE_0;
    }

    for (const named_key& named : NAMED_KEYS) {
        if (strlen(named.name) == length && strncmp(key, named.name, length) == 0) {
            return named.usage;
        }
    }

    return 0;
}

/* F1 to F12 and Print Screen, which follows F12 on the HID keyboard page, type nothing in apps */
static bool types_nothing(uint16_t usage) {
    return usage >= HID_USAGE_F1 && usage <= HID_USAGE_PRINT;
}

bool parse_chord(const char* chord, uint8_t* out_mods, uint16_t* out_usage) {
    uint8_t mods = 0;
    uint16_t usage = 0;

    const char* p = chord;
    while (*p) {
        size_t len = 0;
        const char* next = next_chord_key(p, &len);

        if (len == 4 && strncmp(p, "ctrl", 4) == 0) {
            mods |= CHORD_MOD_CTRL;
        } else if (len == 3 && strncmp(p, "alt", 3) == 0) {
            mods |= CHORD_MOD_ALT;
        } else if (len == 5 && strncmp(p, "shift", 5) == 0) {
            mods |= CHORD_MOD_SHIFT;
        } else if ((len == 3 && strncmp(p, "gui", 3) == 0) ||
                   (len == 5 && strncmp(p, "super", 5) == 0)) {
            mods |= CHORD_MOD_SUPER;
        } else {
            usage = key_usage(p, len);
            if (usage == 0) {
                return false;
            }
        }

        p = next;
    }

    bool takes_typing_key = (mods & COMMAND_MODS) == 0 && !types_nothing(usage);
    if (usage == 0 || takes_typing_key) {
        return false;
    }

    *out_mods = mods;
    *out_usage = usage;
    return true;
}

/* Keys read as printed on a keyboard, so super+f1 shows Super and F1 */
static std::string keycap_name(const std::string& key) {
    if (key == "ctrl") {
        return "Ctrl";
    }

    if (key == "alt") {
        return "Alt";
    }

    if (key == "shift") {
        return "Shift";
    }

    if (key == "gui" || key == "super") {
        return "Super";
    }

    std::string name = key;
    name[0] = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));

    return name;
}

std::vector<std::string> chord_keycaps(const char* chord) {
    uint8_t mods = 0;
    uint16_t usage = 0;
    if (!parse_chord(chord, &mods, &usage)) {
        return {};
    }

    std::vector<std::string> keys;
    for (const char* key = chord; *key;) {
        size_t length = 0;
        const char* next = next_chord_key(key, &length);
        keys.push_back(keycap_name(std::string(key, length)));
        key = next;
    }

    return keys;
}
