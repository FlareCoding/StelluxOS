#include "hid_keyboard_driver.h"
#include <process/process.h>
#include <kprint.h>

HidKeyboardDriver::HidKeyboardDriver() {
    m_previousModifiers = 0;

    for (int i = 0; i < 6; ++i) {
        m_previousKeys[i] = 0;
    }
}

void HidKeyboardDriver::handleEvent(uint8_t* data) {
    uint8_t currentModifiers = data[0];
    uint8_t currentKeys[6];
    for (int i = 0; i < 6; ++i) {
        currentKeys[i] = data[2 + i];
    }

    bool shiftPressed = (currentModifiers & 0x02) || (currentModifiers & 0x20); // Left or Right Shift

    // Handle modifier key changes
    if (currentModifiers != m_previousModifiers) {
        // Detect which modifier keys have changed
        for (uint8_t mask = 1; mask != 0; mask <<= 1) {
            bool wasPressed = m_previousModifiers & mask;
            bool isPressed = currentModifiers & mask;

            if (wasPressed != isPressed) {
                // Handle modifier key press/release
                // You can implement specific actions for each modifier key here
            }
        }
    }

    // Handle key presses
    for (int i = 0; i < 6; ++i) {
        uint8_t keycode = currentKeys[i];
        if (keycode == 0) {
            continue; // No key in this slot
        }

        bool isNewKey = true;
        for (int j = 0; j < 6; ++j) {
            if (keycode == m_previousKeys[j]) {
                isNewKey = false;
                break;
            }
        }

        if (isNewKey) {
            // Key was not in previous report, so it's a new key press
            _handleKeyPress(keycode, shiftPressed);
        }
    }

    // Handle key releases
    for (int i = 0; i < 6; ++i) {
        uint8_t keycode = m_previousKeys[i];
        if (keycode == 0) {
            continue; // No key in this slot
        }

        bool isReleased = true;
        for (int j = 0; j < 6; ++j) {
            if (keycode == currentKeys[j]) {
                isReleased = false;
                break;
            }
        }

        if (isReleased) {
            // Key was in previous report but not in current, so it's released
            _handleKeyRelease(keycode);
        }
    }

    // Update previous state
    m_previousModifiers = currentModifiers;
    for (int i = 0; i < 6; ++i) {
        m_previousKeys[i] = currentKeys[i];
    }
}

void HidKeyboardDriver::_handleKeyPress(uint8_t keycode, bool shiftPressed) {
    char keyChar = _mapKeycodeToChar(keycode, shiftPressed);

    // Handle the key press event
    kprintf("%c\n", keyChar);
}

void HidKeyboardDriver::_handleKeyRelease(uint8_t keycode) {
    char keyChar = _mapKeycodeToChar(keycode, false); // Shift doesn't matter on release

    // Handle the key release event
    __unused keyChar;
}

char HidKeyboardDriver::_mapKeycodeToChar(uint8_t keycode, bool shiftPressed) {
    // Only mapping keycodes from 0x04 to 0x38
    if (keycode < 0x04 || keycode > 0x38) {
        return 0; // Invalid or unmapped keycode
    }

    // Index into the array
    uint8_t index = keycode - 0x04;

    // Unshifted characters
    static const char unshifted[] = {
        // 0x04 - 0x1D: Letters a-z
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', // 0x04 - 0x0B
        'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', // 0x0C - 0x13
        'q', 'r', 's', 't', 'u', 'v', 'w', 'x', // 0x14 - 0x1B
        'y', 'z',                                  // 0x1C - 0x1D

        // 0x1E - 0x27: Numbers 1-0
        '1', '2', '3', '4', '5', '6', '7', '8', // 0x1E - 0x25
        '9', '0',                                 // 0x26 - 0x27

        // 0x28 - 0x38: Special characters and symbols
        '\n',   // Enter (0x28)
        0x1B,   // Escape (0x29)
        '\b',   // Backspace (0x2A)
        '\t',   // Tab (0x2B)
        ' ',    // Space (0x2C)
        '-',    // '-' (0x2D)
        '=',    // '=' (0x2E)
        '[',    // '[' (0x2F)
        ']',    // ']' (0x30)
        '\\',   // '\' (0x31)
        '#',    // Non-US '#' (0x32)
        ';',    // ';' (0x33)
        '\'',   // ''' (0x34)
        '`',    // '`' (0x35)
        ',',    // ',' (0x36)
        '.',    // '.' (0x37)
        '/',    // '/' (0x38)
    };

    // Shifted characters
    static const char shifted[] = {
        // 0x04 - 0x1D: Letters A-Z
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', // 0x04 - 0x0B
        'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', // 0x0C - 0x13
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', // 0x14 - 0x1B
        'Y', 'Z',                                  // 0x1C - 0x1D

        // 0x1E - 0x27: Symbols shifted from numbers
        '!', '@', '#', '$', '%', '^', '&', '*', // 0x1E - 0x25
        '(', ')',                                 // 0x26 - 0x27

        // 0x28 - 0x38: Special characters and shifted symbols
        '\n',   // Enter (0x28)
        0x1B,   // Escape (0x29)
        '\b',   // Backspace (0x2A)
        '\t',   // Tab (0x2B)
        ' ',    // Space (0x2C)
        '_',    // '_' (0x2D)
        '+',    // '+' (0x2E)
        '{',    // '{' (0x2F)
        '}',    // '}' (0x30)
        '|',    // '|' (0x31)
        '~',    // Non-US '~' (0x32)
        ':',    // ':' (0x33)
        '"',    // '"' (0x34)
        '~',    // '~' (0x35)
        '<',    // '<' (0x36)
        '>',    // '>' (0x37)
        '?',    // '?' (0x38)
    };

    char c = shiftPressed ? shifted[index] : unshifted[index];
    return c;
}

