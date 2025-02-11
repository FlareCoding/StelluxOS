#ifndef COLOR_H
#define COLOR_H
#include <types.h>
#include <core/string.h>

namespace stella_ui {
class color {
public:
    color(); // Default constructor (black, fully opaque)
    color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
    explicit color(uint32_t argb);

    static color from_hex(const kstl::string& hex);

    uint32_t to_argb() const;
    uint8_t alpha() const;
    uint8_t r() const;
    uint8_t g() const;
    uint8_t b() const;

    // Predefined common colors
    static const color black;
    static const color white;
    static const color red;
    static const color green;
    static const color blue;
    static const color yellow;
    static const color cyan;
    static const color magenta;
    static const color gray;
    static const color dark_gray;
    static const color transparent;

private:
    uint32_t m_argb;

    static uint8_t _parse_component(const kstl::string& str, size_t start, size_t length);
    static uint32_t _hex_to_uint32(const kstl::string& hex);
};
} // namespace stella_ui

#endif // COLOR_H
