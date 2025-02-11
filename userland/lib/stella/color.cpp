#include "color.h"

namespace stella_ui {
const color color::black        = color(0, 0, 0);
const color color::white        = color(255, 255, 255);
const color color::red          = color(255, 0, 0);
const color color::green        = color(0, 255, 0);
const color color::blue         = color(0, 0, 255);
const color color::yellow       = color(255, 255, 0);
const color color::cyan         = color(0, 255, 255);
const color color::magenta      = color(255, 0, 255);
const color color::gray         = color(128, 128, 128);
const color color::dark_gray    = color(60, 60, 60);
const color color::transparent  = color(0, 0, 0, 0);

// Default constructor (black, fully opaque)
color::color() : m_argb(0xff000000) {}

// Initialize with individual components (r, g, b, a)
color::color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    m_argb = (static_cast<uint32_t>(a) << 24) | 
             (static_cast<uint32_t>(r) << 16) | 
             (static_cast<uint32_t>(g) << 8)  | 
             static_cast<uint32_t>(b);
}

// Initialize directly with 0xAARRGGBB
color::color(uint32_t argb) : m_argb(argb) {}

// Create color from a hex string (e.g., "#RRGGBB" or "#AARRGGBB")
color color::from_hex(const kstl::string& hex) {
    uint32_t argb = _hex_to_uint32(hex);
    return color(argb);
}

// Return the color as a 0xAARRGGBB integer
uint32_t color::to_argb() const {
    return m_argb;
}

// Extract individual components
uint8_t color::alpha() const {
    return (m_argb >> 24) & 0xff;
}

uint8_t color::r() const {
    return (m_argb >> 16) & 0xff;
}

uint8_t color::g() const {
    return (m_argb >> 8) & 0xff;
}

uint8_t color::b() const {
    return m_argb & 0xff;
}

// Helper: Parse hex string into a 32-bit color (supporting #RRGGBB and #AARRGGBB formats)
uint32_t color::_hex_to_uint32(const kstl::string& hex) {
    size_t length = hex.length();
    if (length != 7 && length != 9) {
        // Invalid format (neither #RRGGBB nor #AARRGGBB)
        return 0xff000000;  // Default to opaque black
    }

    uint32_t result = 0;
    size_t start = (hex[0] == '#') ? 1 : 0;  // Skip '#' if present

    for (size_t i = 0; i < length - start; i++) {
        result = (result << 4) | _parse_component(hex, start + i, 1);
    }

    // For #RRGGBB, prepend full alpha (0xff)
    if (length == 7) {
        result |= 0xff000000;
    }

    return result;
}

uint8_t color::_parse_component(const kstl::string& str, size_t start, size_t length) {
    uint8_t value = 0;
    for (size_t i = 0; i < length; i++) {
        char c = str[start + i];
        value = (value << 4) | (c >= '0' && c <= '9' ? c - '0' :
                                c >= 'A' && c <= 'F' ? c - 'A' + 10 :
                                c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0);
    }
    return value;
}
} // namespace stella_ui

