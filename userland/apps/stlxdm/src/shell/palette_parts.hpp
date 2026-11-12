#ifndef STLXDM_SHELL_PALETTE_PARTS_HPP
#define STLXDM_SHELL_PALETTE_PARTS_HPP

#include "draw.hpp"

#include <stlxtheme/theme.h>
#include <stlxui/stlxui.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

constexpr int32_t HEAD_HEIGHT = 28;
constexpr int32_t HEAD_PADDING_X = 10;
constexpr int32_t HEAD_PADDING_BOTTOM = 5;
constexpr int32_t SUBTITLE_LINE_HEIGHT = 16;

constexpr std::string_view ELLIPSIS = "\xE2\x80\xA6";

/* One action the footer names, and the keys that run it, space separated */
struct footer_item {
    std::string label;
    std::string keys;

    bool operator==(const footer_item& other) const = default;
};

/* A section's title in caps, and at the right a count or a state in sentence case */
class section_head : public ui::widget {
public:
    explicit section_head(const char* title)
        : m_title(title, STLXTHEME_UI_FONT_SEMIBOLD_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM),
          m_end("", STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM) {}

    void set_end(const std::string& text) {
        if (m_end.set_text(text)) {
            invalidate();
        }
    }

    ui::size measure(ui::size avail) override {
        return { avail.w, HEAD_HEIGHT };
    }

    void paint(ui::painter& p) override {
        int32_t top = HEAD_HEIGHT - HEAD_PADDING_BOTTOM - SUBTITLE_LINE_HEIGHT;
        m_title.draw(p, HEAD_PADDING_X, top, SUBTITLE_LINE_HEIGHT);
        m_end.draw(p, m_frame.w - HEAD_PADDING_X - m_end.width(), top, SUBTITLE_LINE_HEIGHT);
    }

private:
    text_picture m_title;
    text_picture m_end;
};

/* The bytes of `text` that fit `max_w` pixels in the face, cut at a character boundary with room left for an
 * ellipsis when the whole does not fit */
size_t fitting_length(const std::string& text, const char* face_path, float em_px, int32_t max_w);

/* `text` as it fits `max_w` pixels in the face, ending in an ellipsis when cut */
std::string fitted(const std::string& text, const char* face_path, float em_px, int32_t max_w);

std::string_view trimmed(std::string_view text);
const char* file_name(const char* path);

/* Bytes as people read them, to one decimal past a thousand */
std::string size_text(uint64_t bytes);

#endif
