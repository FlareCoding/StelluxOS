#include "chord.hpp"

#include <cctype>
#include <cstring>

constexpr uint16_t HID_USAGE_A = 0x04;
constexpr uint16_t HID_USAGE_1 = 0x1E;
constexpr uint16_t HID_USAGE_0 = 0x27;

/* Gives the length of the key at `key` and returns where the chord's next key starts */
static const char* next_chord_key(const char* key, size_t* length) {
    const char* end = strchr(key, '+');
    *length = end ? static_cast<size_t>(end - key) : strlen(key);

    return end ? end + 1 : key + *length;
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
        } else if (len == 1 && *p >= 'a' && *p <= 'z') {
            usage = static_cast<uint16_t>(HID_USAGE_A + (*p - 'a'));
        } else if (len == 1 && *p >= '1' && *p <= '9') {
            usage = static_cast<uint16_t>(HID_USAGE_1 + (*p - '1'));
        } else if (len == 1 && *p == '0') {
            usage = HID_USAGE_0;
        } else {
            return false;
        }

        p = next;
    }

    if (usage == 0) {
        return false;
    }

    *out_mods = mods;
    *out_usage = usage;
    return true;
}

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

    return std::string(1, static_cast<char>(toupper(static_cast<unsigned char>(key[0]))));
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
