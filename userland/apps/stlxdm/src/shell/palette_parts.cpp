#include "palette_parts.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

size_t fitting_length(const std::string& text, const char* face_path, float em_px, int32_t max_w) {
    stlxgfx_font* font = shell_font(face_path, em_px);
    if (!font || stlxgfx_text_width(font, text.data(), text.size()) <= max_w) {
        return text.size();
    }

    /* The cut falls on a character's first byte, found by halves, so a long text costs a few measurements */
    std::vector<size_t> cuts;
    for (size_t i = 1; i < text.size(); i++) {
        if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) {
            cuts.push_back(i);
        }
    }

    int32_t room = max_w - stlxgfx_text_width(font, ELLIPSIS.data(), ELLIPSIS.size());
    auto first_over = std::partition_point(cuts.begin(), cuts.end(), [&](size_t cut) {
        return stlxgfx_text_width(font, text.data(), cut) <= room;
    });

    return first_over == cuts.begin() ? 0 : *(first_over - 1);
}

std::string fitted(const std::string& text, const char* face_path, float em_px, int32_t max_w) {
    size_t length = fitting_length(text, face_path, em_px, max_w);
    return length == text.size() ? text : text.substr(0, length) + std::string(ELLIPSIS);
}

std::string_view trimmed(std::string_view text) {
    size_t first = text.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        return {};
    }

    size_t last = text.find_last_not_of(' ');
    return text.substr(first, last - first + 1);
}

const char* file_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

std::string size_text(uint64_t bytes) {
    static const char* const UNITS[] = { "KB", "MB", "GB", "TB" };
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    }

    double size = static_cast<double>(bytes) / 1024;
    size_t unit = 0;
    while (size >= 1024 && unit + 1 < std::size(UNITS)) {
        size /= 1024;
        unit++;
    }

    char text[32];
    snprintf(text, sizeof(text), "%.1f %s", size, UNITS[unit]);

    return text;
}
