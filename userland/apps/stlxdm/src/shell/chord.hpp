#ifndef STLXDM_SHELL_CHORD_HPP
#define STLXDM_SHELL_CHORD_HPP

#include <cstdint>
#include <string>
#include <vector>

/* The modifier bits a chord parses to, the bits the server cooks from the HID modifiers */
constexpr uint8_t CHORD_MOD_SHIFT = 1u << 0;
constexpr uint8_t CHORD_MOD_CTRL = 1u << 1;
constexpr uint8_t CHORD_MOD_ALT = 1u << 2;
constexpr uint8_t CHORD_MOD_SUPER = 1u << 3;

/* Parses a plus-separated chord such as ctrl+alt+t or shift+print into modifier bits and the HID usage of its key.
 * Refuses unknown words, a chord with no key, and a key that types held without Ctrl, Alt or Super, which apps need. */
bool parse_chord(const char* chord, uint8_t* out_mods, uint16_t* out_usage);

/* The names keycaps show for the keys of `chord`, such as Super and S, or none when `parse_chord` refuses it */
std::vector<std::string> chord_keycaps(const char* chord);

#endif
