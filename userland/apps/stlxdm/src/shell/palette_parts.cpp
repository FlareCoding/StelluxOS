#include "palette_parts.hpp"

#include <cstdio>
#include <cstring>
#include <iterator>

size_t fitting_length(const std::string& text, const char* face_path, float em_px, int32_t max_w) {
    stlxgfx_font* font = shell_font(face_path, em_px);
    if (!font || stlxgfx_text_width(font, text.data(), text.size()) <= max_w) {
        return text.size();
    }

    int32_t ellipsis_w = stlxgfx_text_width(font, ELLIPSIS.data(), ELLIPSIS.size());
    size_t end = text.size();
    while (end > 0) {
        end--;
        while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) {
            end--;
        }

        if (stlxgfx_text_width(font, text.data(), end) + ellipsis_w <= max_w) {
            break;
        }
    }

    return end;
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
