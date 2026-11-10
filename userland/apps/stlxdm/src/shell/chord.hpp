#ifndef STLXDM_SHELL_CHORD_HPP
#define STLXDM_SHELL_CHORD_HPP

#include <cstdint>

/* The modifier bits a chord parses to, the bits the server cooks from the HID modifiers */
constexpr uint8_t CHORD_MOD_SHIFT = 1u << 0;
constexpr uint8_t CHORD_MOD_CTRL = 1u << 1;
constexpr uint8_t CHORD_MOD_ALT = 1u << 2;
constexpr uint8_t CHORD_MOD_SUPER = 1u << 3;

/* Parses a plus-separated chord such as ctrl+alt+t into modifier bits and the HID usage of its last letter or
 * digit. Returns false when a key is neither a modifier name nor one lowercase letter or digit, or when no
 * key is a letter or digit. */
bool parse_chord(const char* chord, uint8_t* out_mods, uint16_t* out_usage);

#endif
