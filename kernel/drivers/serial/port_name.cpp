#include "drivers/serial/port_name.h"
#include "common/string.h"

namespace serial {

// A port's first name has this many letters or digits of its serial number, and each collision adds more
static constexpr size_t SERIAL_CHARACTERS_IN_FIRST_NAME = 4;
static constexpr size_t SERIAL_CHARACTERS_PER_COLLISION = 2;

static constexpr char LOCATION_PREFIX[] = "-p";

// A channel's letter runs from 'a' to 'z'
static constexpr int8_t CHANNEL_LETTERS = 26;

static bool is_letter_or_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char to_lowercase(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

static size_t count_letters_and_digits(const char* text) {
    size_t count = 0;
    for (const char* c = text; *c != '\0'; c++) {
        if (is_letter_or_digit(*c)) {
            count++;
        }
    }

    return count;
}

// Counts `c` even where it does not fit, so a name that is too long can be refused
static size_t append_character(char* name, size_t length, char c) {
    if (length < PORT_NAME_MAX) {
        name[length] = c;
    }

    return length + 1;
}

static size_t append_text(char* name, size_t length, const char* text) {
    for (const char* c = text; *c != '\0'; c++) {
        length = append_character(name, length, *c);
    }

    return length;
}

static size_t append_serial_tail(char* name, size_t length, const char* serial_number, size_t serial_characters) {
    size_t to_skip = count_letters_and_digits(serial_number) - serial_characters;
    for (const char* c = serial_number; *c != '\0'; c++) {
        if (!is_letter_or_digit(*c)) {
            continue;
        }

        if (to_skip > 0) {
            to_skip--;
            continue;
        }

        length = append_character(name, length, to_lowercase(*c));
    }

    return length;
}

static size_t format_base(const identity& id, size_t serial_characters, bool with_location, char* name) {
    size_t length = append_text(name, 0, id.chip);
    if (serial_characters > 0) {
        length = append_character(name, length, '-');
        length = append_serial_tail(name, length, id.serial_number, serial_characters);
    }

    if (with_location) {
        length = append_text(name, length, LOCATION_PREFIX);
        length = append_text(name, length, id.location);
    }

    return length;
}

// The serial number characters in the next base, or 0 to stop once a base had all `available`
static size_t next_serial_character_count(size_t serial_characters, size_t available) {
    if (serial_characters == available) {
        return 0;
    }

    size_t grown = serial_characters + SERIAL_CHARACTERS_PER_COLLISION;

    return grown < available ? grown : available;
}

static bool is_same_device(const identity& a, const identity& b) {
    return string::strcmp(a.chip, b.chip) == 0 &&
           string::strcmp(a.serial_number, b.serial_number) == 0 &&
           string::strcmp(a.location, b.location) == 0;
}

static const held_name* find_device_name(held_name_list& held, const identity& id) {
    for (held_name& name : held) {
        if (is_same_device(name.device, id)) {
            return &name;
        }
    }

    return nullptr;
}

static bool is_base_held(held_name_list& held, const char* base, size_t length) {
    for (held_name& name : held) {
        if (name.base_length == length && string::memcmp(name.text, base, length) == 0) {
            return true;
        }
    }

    return false;
}

static bool is_name_held(held_name_list& held, const char* text) {
    for (held_name& name : held) {
        if (string::strcmp(name.text, text) == 0) {
            return true;
        }
    }

    return false;
}

// Writes the first base no port holds into `name`, for a device none of whose ports holds a name
static bool choose_base(const identity& id, held_name_list& held, char* name, size_t* length) {
    size_t available = count_letters_and_digits(id.serial_number);

    size_t first = available < SERIAL_CHARACTERS_IN_FIRST_NAME ? available : SERIAL_CHARACTERS_IN_FIRST_NAME;
    for (size_t serial_characters = first; serial_characters > 0;
         serial_characters = next_serial_character_count(serial_characters, available)) {
        *length = format_base(id, serial_characters, false, name);
        if (*length > PORT_NAME_MAX) {
            return false;
        }

        if (!is_base_held(held, name, *length)) {
            return true;
        }
    }

    if (id.location[0] == '\0') {
        return false;
    }

    *length = format_base(id, available, true, name);

    return *length <= PORT_NAME_MAX && !is_base_held(held, name, *length);
}

bool choose_port_name(const identity& id, held_name_list& held, held_name& chosen) {
    if (id.channel >= CHANNEL_LETTERS) {
        return false;
    }

    size_t base_length = 0;
    const held_name* sibling = find_device_name(held, id);
    if (sibling) {
        base_length = sibling->base_length;
        string::memcpy(chosen.text, sibling->text, base_length);
    } else if (!choose_base(id, held, chosen.text, &base_length)) {
        return false;
    }

    size_t length = base_length;
    if (id.channel >= 0) {
        length = append_character(chosen.text, length, '.');
        length = append_character(chosen.text, length, static_cast<char>('a' + id.channel));
    }

    if (length > PORT_NAME_MAX) {
        return false;
    }

    chosen.text[length] = '\0';

    // A device that numbers two of its ports alike must not give them one name
    if (is_name_held(held, chosen.text)) {
        return false;
    }

    chosen.device = id;
    chosen.base_length = base_length;

    return true;
}

} // namespace serial
