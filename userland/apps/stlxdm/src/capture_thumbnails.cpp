#include "capture_thumbnails.hpp"

#include "shell/draw.hpp"
#include "shell/popover.hpp"

#include <stlxgfx/ctx.h>
#include <stlxgfx/font.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <cmath>

constexpr size_t CARDS_MAX = 3;
constexpr uint64_t SHOWN_NS = 6000000000ull;
constexpr uint64_t AFTER_HOVER_NS = 2000000000ull;
constexpr uint64_t SLIDE_NS = 200000000ull;
constexpr uint64_t SLIDE_FRAME_NS = 4000000ull;

/* A card is its capture fitted inside 240 by 160, never grown, over one caption line, so a wide capture makes a short
 * card and three screens stack to under half a 1080 px display */
constexpr int32_t IMAGE_MAX_W = 240;
constexpr int32_t IMAGE_MAX_H = 160;
constexpr int32_t IMAGE_RADIUS = STLXTHEME_CONTROL_RADIUS;
constexpr int32_t CARD_PADDING = 4;
constexpr int32_t CARD_MARGIN = 16;
constexpr int32_t CARD_WIDTH = IMAGE_MAX_W + 2 * CARD_PADDING;
constexpr int32_t CAPTION_HEIGHT = 30;
constexpr int32_t CAPTION_LEFT = 6;

/* The actions are round buttons on dark faces along the image's top right corner, clear of its middle, or at the
 * caption's end under an image too short to hold them */
constexpr int32_t ACTION_SIZE = 24;
constexpr int32_t ACTION_GAP = 4;
constexpr int32_t ACTION_INSET = 6;
constexpr int32_t ACTION_GLYPH = 14;
constexpr int32_t ACTIONS_OVER_MIN_H = 64;
constexpr uint32_t BUTTON_FACE = 0xE6181511u;
constexpr uint32_t BUTTON_FACE_LIT = 0xF2363028u;

/* The tooltip is glass in a hairline, as the dock's */
constexpr int32_t TIP_HEIGHT = 24;
constexpr int32_t TIP_PADDING_X = 8;
constexpr int32_t TIP_GAP = 8;

constexpr const char* PREVIEW_TIP = "Preview";
constexpr const char* DISCARD_TIP = "Discard, and put back what was copied before";
constexpr const char* DISCARD_ONLY_TIP = "Discard";

/* A capture's card: the capture fitted to it, its kind and size, its place on the desktop, and its pixels, which the
 * encoder holds until it hands them back and the card keeps from then on */
struct thumbnail_card {
    uint64_t                       job = 0;
    uint64_t                       entry = 0;
    capture_kind                   kind = capture_kind::screen;
    int32_t                        width = 0;
    int32_t                        height = 0;
    uint32_t                       output = 0;
    damage_list::rect              place = {};      /* its output's rect on the desktop */
    damage_list::rect              box = {};        /* the card on the desktop, its shadow left out */
    damage_list::rect              glass_box = {};  /* where its picture's glass was drawn from */
    stlxgfx_surface_t*             image = nullptr; /* the capture fitted to the card */
    const stlxgfx_surface_t*       pixels = nullptr;
    stlxgfx_surface_t*             kept = nullptr;
    bool                           keeps_alpha = false;
    std::unique_ptr<panel_picture> picture;
    bool                           is_current = true;
    bool                           restores = false; /* whether its Discard would put back the copy before it */
    uint64_t                       shown_ns = 0;
    uint64_t                       deadline_ns = 0;
    int32_t                        slide = 0;       /* how far right of its box it still is as it slides in */

    thumbnail_card() = default;
    ~thumbnail_card() {
        stlxgfx_destroy_surface(image);
        stlxgfx_destroy_surface(kept);
    }

    thumbnail_card(const thumbnail_card&) = delete;
    thumbnail_card& operator=(const thumbnail_card&) = delete;
};

/* A trash can on the 16 unit grid: its lid with a handle, and its body open at the top with two ribs */
static std::vector<std::vector<glyph_point>> discard_contours() {
    return {
        { { 2.0f, 3.5f }, { 14.0f, 3.5f }, { 14.0f, 4.75f }, { 2.0f, 4.75f } },
        { { 5.5f, 1.25f }, { 10.5f, 1.25f }, { 10.5f, 3.5f }, { 5.5f, 3.5f } },
        { { 6.75f, 2.5f }, { 9.25f, 2.5f }, { 9.25f, 3.5f }, { 6.75f, 3.5f } },
        { { 3.25f, 5.75f }, { 12.75f, 5.75f }, { 12.0f, 14.75f }, { 4.0f, 14.75f } },
        { { 4.5f, 5.75f }, { 11.5f, 5.75f }, { 10.85f, 13.5f }, { 5.15f, 13.5f } },
        { { 6.4f, 7.5f }, { 7.6f, 7.5f }, { 7.6f, 11.75f }, { 6.4f, 11.75f } },
        { { 8.4f, 7.5f }, { 9.6f, 7.5f }, { 9.6f, 11.75f }, { 8.4f, 11.75f } },
    };
}

/* An almond on the 16 unit grid between two parabolas meeting at its tips, `reach` from its middle */
static std::vector<glyph_point> almond(float reach, float rise) {
    constexpr int32_t STEPS = 12;
    std::vector<glyph_point> outline;
    for (int32_t i = 0; i <= STEPS; i++) {
        float t = -1.0f + 2.0f * static_cast<float>(i) / STEPS;
        outline.push_back({ 8.0f + reach * t, 8.0f - rise * (1.0f - t * t) });
    }

    for (int32_t i = STEPS - 1; i > 0; i--) {
        float t = -1.0f + 2.0f * static_cast<float>(i) / STEPS;
        outline.push_back({ 8.0f + reach * t, 8.0f + rise * (1.0f - t * t) });
    }

    return outline;
}

/* An eye on the 16 unit grid: an almond's outline around its pupil */
static std::vector<std::vector<glyph_point>> preview_contours() {
    constexpr int32_t SIDES = 16;
    constexpr float PUPIL = 2.4f;
    std::vector<glyph_point> pupil;
    for (int32_t i = 0; i < SIDES; i++) {
        float angle = 6.2831853f * static_cast<float>(i) / SIDES;
        pupil.push_back({ 8.0f + PUPIL * std::cos(angle), 8.0f + PUPIL * std::sin(angle) });
    }

    return { almond(7.0f, 5.0f), almond(5.6f, 3.6f), pupil };
}

/* A cross on the 16 unit grid as one polygon, a plus turned an eighth, so the evenodd rule keeps its middle */
static std::vector<std::vector<glyph_point>> close_contours() {
    constexpr float HALF = 0.75f;
    constexpr float REACH = 6.0f;
    constexpr float TURN = 0.70710678f;
    const float plus[12][2] = { { HALF, REACH },  { HALF, HALF },    { REACH, HALF },   { REACH, -HALF },
                                { HALF, -HALF },  { HALF, -REACH },  { -HALF, -REACH }, { -HALF, -HALF },
                                { -REACH, -HALF }, { -REACH, HALF }, { -HALF, HALF },   { -HALF, REACH } };

    std::vector<glyph_point> cross;
    for (const auto& corner : plus) {
        cross.push_back({ 8.0f + (corner[0] - corner[1]) * TURN, 8.0f + (corner[0] + corner[1]) * TURN });
    }

    return { cross };
}

/* The capture shrunk to fit the card's image, never grown, its alpha kept so a window's shadow falls on the glass */
static stlxgfx_surface_t* make_image(const stlxgfx_surface_t* pixels, bool keeps_alpha) {
    double scale = std::min({ 1.0, static_cast<double>(IMAGE_MAX_W) / pixels->width,
                              static_cast<double>(IMAGE_MAX_H) / pixels->height });
    int32_t w = std::max(1, static_cast<int32_t>(pixels->width * scale + 0.5));
    int32_t h = std::max(1, static_cast<int32_t>(pixels->height * scale + 0.5));
    return shrink_capture(pixels, keeps_alpha, w, h);
}

static const char* kind_name(capture_kind kind) {
    switch (kind) {
    case capture_kind::screen: return "Screen";
    case capture_kind::region: return "Region";
    case capture_kind::window: return "Window";
    }

    return "Capture";
}

/* A card's caption, its kind and size as "Region \xC2\xB7 758 \xC3\x97 330" */
static std::string title_of(const thumbnail_card& card) {
    return std::string(kind_name(card.kind)) + " \xC2\xB7 " + std::to_string(card.width) + " \xC3\x97 " +
           std::to_string(card.height);
}

/* Where the image sits on a card's picture content: across its middle, under its padding */
static damage_list::rect image_rect(const thumbnail_card& card) {
    int32_t w = static_cast<int32_t>(card.image->width);
    return { (CARD_WIDTH - w) / 2, CARD_PADDING, w, static_cast<int32_t>(card.image->height) };
}

/* A card slides in from past its output's right edge, fast at first and settling at its place */
static int32_t slide_at(uint64_t elapsed_ns) {
    if (elapsed_ns >= SLIDE_NS) {
        return 0;
    }

    float rest = 1.0f - static_cast<float>(elapsed_ns) / static_cast<float>(SLIDE_NS);
    return static_cast<int32_t>(static_cast<float>(CARD_WIDTH + CARD_MARGIN) * rest * rest * rest + 0.5f);
}

static bool contains(const damage_list::rect& r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* One line of text centered in the `box_h` rows below `top` */
static void draw_line(stlxgfx_surface_t* target, const char* face, float size, int32_t x, int32_t top, int32_t box_h,
                      const std::string& text, uint32_t color) {
    stlxgfx_font* font = shell_font(face, size);
    if (!font) {
        return;
    }

    stlxgfx_font_metrics metrics;
    stlxgfx_font_metrics_get(font, &metrics);
    int32_t baseline = top + (box_h + metrics.ascent - metrics.descent) / 2;
    stlxgfx_draw_text(target, font, x, baseline, text.data(), text.size(), color);
}

/* A card's height: its image and the caption line under it */
static int32_t card_height(const thumbnail_card& card) {
    return CARD_PADDING + static_cast<int32_t>(card.image->height) + CAPTION_HEIGHT;
}

/* Where the actions sit on a card's picture content, from the right: close, Discard, then Preview, along the image's top
 * right corner, or at the caption's end when the image is too short to keep them off its middle */
static damage_list::rect action_rect(const thumbnail_card& card, int32_t slot) {
    damage_list::rect image = image_rect(card);
    bool is_under = image.h < ACTIONS_OVER_MIN_H;
    int32_t top = is_under ? image.y + image.h + (CAPTION_HEIGHT - ACTION_SIZE) / 2 : image.y + ACTION_INSET;
    int32_t right = CARD_WIDTH - CARD_PADDING - (is_under ? 0 : ACTION_INSET);
    return { right - (slot + 1) * ACTION_SIZE - slot * ACTION_GAP, top, ACTION_SIZE, ACTION_SIZE };
}

static damage_list::rect close_rect(const thumbnail_card& card) {
    return action_rect(card, 0);
}

static damage_list::rect discard_rect(const thumbnail_card& card) {
    return action_rect(card, 1);
}

static damage_list::rect preview_rect(const thumbnail_card& card) {
    return action_rect(card, 2);
}

static void draw_glyph(stlxgfx_surface_t* target, const stlxgfx_surface_t* glyph, const damage_list::rect& r) {
    if (glyph) {
        stlxgfx_blit_alpha(target, r.x + (r.w - static_cast<int32_t>(glyph->width)) / 2,
                           r.y + (r.h - static_cast<int32_t>(glyph->height)) / 2, glyph, 0, 0, glyph->width,
                           glyph->height);
    }
}

/* A round action button at `r`, lit under the pointer */
static void draw_action(stlxgfx_ctx_t* ctx, stlxgfx_surface_t* target, const damage_list::rect& r, bool is_lit,
                        const stlxgfx_surface_t* glyph) {
    int32_t radius = r.w / 2;
    stlxgfx_ctx_fill_circle(ctx, r.x + radius, r.y + radius, radius, is_lit ? BUTTON_FACE_LIT : BUTTON_FACE);
    stlxgfx_ctx_draw_rounded_rect(ctx, r.x, r.y, static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h), radius,
                                  is_lit ? STLXTHEME_LINE_STRONG : STLXTHEME_LINE);
    draw_glyph(target, glyph, r);
}

capture_thumbnails::capture_thumbnails()
    : m_preview_glyph(make_shape_glyph(ACTION_GLYPH, STLXTHEME_TEXT_SECONDARY, preview_contours())),
      m_preview_glyph_lit(make_shape_glyph(ACTION_GLYPH, STLXTHEME_TEXT, preview_contours())),
      m_discard_glyph(make_shape_glyph(ACTION_GLYPH, STLXTHEME_TEXT_SECONDARY, discard_contours())),
      m_discard_glyph_lit(make_shape_glyph(ACTION_GLYPH, STLXTHEME_TEXT, discard_contours())),
      m_close_glyph(make_shape_glyph(ACTION_GLYPH, STLXTHEME_TEXT_SECONDARY, close_contours())),
      m_close_glyph_lit(make_shape_glyph(ACTION_GLYPH, STLXTHEME_TEXT, close_contours())) {
}

capture_thumbnails::~capture_thumbnails() {
    for (stlxgfx_surface_t* glyph : { m_preview_glyph, m_preview_glyph_lit, m_discard_glyph, m_discard_glyph_lit,
                                      m_close_glyph, m_close_glyph_lit, m_tip }) {
        stlxgfx_destroy_surface(glyph);
    }
}

void capture_thumbnails::add(uint64_t job, capture_kind kind, const stlxgfx_surface_t* pixels, bool keeps_alpha,
                             uint32_t output, const damage_list::rect& place, uint64_t now_ns, damage_list& damage) {
    auto card = std::make_unique<thumbnail_card>();
    card->image = make_image(pixels, keeps_alpha);
    if (!card->image) {
        return;
    }

    card->job = job;
    card->kind = kind;
    card->width = static_cast<int32_t>(pixels->width);
    card->height = static_cast<int32_t>(pixels->height);
    card->output = output;
    card->place = place;

    card->pixels = pixels;
    card->keeps_alpha = keeps_alpha;
    card->shown_ns = now_ns;
    card->deadline_ns = now_ns + SHOWN_NS;
    card->slide = slide_at(0);

    if (m_cards.size() == CARDS_MAX) {
        erase(0, damage);
    }

    m_cards.push_back(std::move(card));
    lay_out(damage);
}

bool capture_thumbnails::encoded(uint64_t job, uint64_t entry, stlxgfx_surface_t* pixels) {
    for (std::unique_ptr<thumbnail_card>& card : m_cards) {
        if (card->job == job) {
            card->entry = entry;
            card->kept = pixels;
            return true;
        }
    }

    return false;
}

bool capture_thumbnails::preview_of(uint64_t job, preview_source* out) const {
    for (const std::unique_ptr<thumbnail_card>& card : m_cards) {
        if (card->job == job) {
            *out = { card->pixels, card->keeps_alpha, title_of(*card), card->is_current, card->output };
            return true;
        }
    }

    return false;
}

thumbnail_click capture_thumbnails::discard_of(uint64_t job) {
    for (const std::unique_ptr<thumbnail_card>& card : m_cards) {
        if (card->job != job) {
            continue;
        }

        if (card->entry == 0) {
            m_dropped.push_back(job);
        }

        return { thumbnail_action::discard, job, card->entry };
    }

    return {};
}

const char* capture_thumbnails::kind_name_of(uint64_t job) const {
    for (const std::unique_ptr<thumbnail_card>& card : m_cards) {
        if (card->job == job) {
            return kind_name(card->kind);
        }
    }

    return nullptr;
}

bool capture_thumbnails::take_dropped(uint64_t job) {
    auto dropped = std::find(m_dropped.begin(), m_dropped.end(), job);
    if (dropped == m_dropped.end()) {
        return false;
    }

    m_dropped.erase(dropped);
    return true;
}

void capture_thumbnails::remove(uint64_t job, damage_list& damage) {
    for (size_t i = 0; i < m_cards.size(); i++) {
        if (m_cards[i]->job == job) {
            erase(i, damage);
            lay_out(damage);
            return;
        }
    }
}

void capture_thumbnails::clear(damage_list& damage) {
    while (!m_cards.empty()) {
        erase(m_cards.size() - 1, damage);
    }
}

/* An entry deleted, cleared or evicted leaves nothing of its capture to discard or describe */
void capture_thumbnails::follow_clipboard(const clipboard_history& history, damage_list& damage) {
    bool is_any_gone = false;
    for (size_t i = m_cards.size(); i-- > 0;) {
        thumbnail_card& card = *m_cards[i];
        if (card.entry != 0 && !history.find(card.entry)) {
            erase(i, damage);
            is_any_gone = true;
            continue;
        }

        bool is_current = card.entry == 0 || history.is_current(card.entry);
        card.restores = card.entry != 0 && history.restores_on_discard(card.entry);
        if (is_current != card.is_current) {
            card.is_current = is_current;
            paint(card);
            damage_card(card, damage);
        }
    }

    if (is_any_gone) {
        lay_out(damage);
    }

    show_tip(damage);
}

bool capture_thumbnails::contains(int32_t x, int32_t y) const {
    return card_at(x, y) >= 0;
}

/* The card under the pointer shows its actions and holds its time, and the action under it lights with its tooltip */
void capture_thumbnails::pointer_move(int32_t x, int32_t y, uint64_t now_ns, damage_list& damage) {
    int32_t hover = card_at(x, y);
    part hover_part = hover >= 0 ? part_at(*m_cards[static_cast<size_t>(hover)], x, y) : part::none;
    if (hover == m_hover && hover_part == m_hover_part) {
        return;
    }

    if (m_hover >= 0 && hover < 0) {
        for (std::unique_ptr<thumbnail_card>& card : m_cards) {
            card->deadline_ns = std::max(card->deadline_ns, now_ns + AFTER_HOVER_NS);
        }
    }

    int32_t was = m_hover;
    m_hover = hover;
    m_hover_part = hover_part;
    for (int32_t changed : { was, hover }) {
        if (changed >= 0) {
            paint(*m_cards[static_cast<size_t>(changed)]);
            damage_card(*m_cards[static_cast<size_t>(changed)], damage);
        }
    }

    show_tip(damage);
}

void capture_thumbnails::press(int32_t x, int32_t y, uint64_t now_ns, damage_list& damage) {
    pointer_move(x, y, now_ns, damage);
    m_pressed = m_hover;
    m_pressed_part = m_hover_part;
}

/* A release over the part its press took acts: close takes the card down here, and Preview, a click on the image and
 * Discard go to the server */
thumbnail_click capture_thumbnails::release(int32_t x, int32_t y, uint64_t now_ns, damage_list& damage) {
    pointer_move(x, y, now_ns, damage);
    bool is_click = m_pressed >= 0 && m_pressed == m_hover && m_pressed_part == m_hover_part;
    int32_t pressed = m_pressed;
    part pressed_part = m_pressed_part;

    m_pressed = -1;
    m_pressed_part = part::none;
    if (!is_click) {
        return {};
    }

    thumbnail_card& card = *m_cards[static_cast<size_t>(pressed)];
    if (pressed_part == part::close) {
        remove(card.job, damage);
        pointer_move(x, y, now_ns, damage);
        return {};
    }

    if (pressed_part == part::preview || pressed_part == part::image) {
        return { thumbnail_action::preview, card.job, card.entry };
    }

    return pressed_part == part::discard ? discard_of(card.job) : thumbnail_click{};
}

/* A sliding card wants its next frame, and the pointer on any card holds them all, so the stack never shifts under it */
int64_t capture_thumbnails::timeout_ns(uint64_t now_ns) const {
    bool is_held = m_hover >= 0 || m_pressed >= 0;
    int64_t soonest = -1;
    for (const std::unique_ptr<thumbnail_card>& card : m_cards) {
        int64_t left = card->deadline_ns > now_ns ? static_cast<int64_t>(card->deadline_ns - now_ns) : 0;
        left = card->slide > 0 ? static_cast<int64_t>(SLIDE_FRAME_NS) : is_held ? -1 : left;
        soonest = soonest < 0 ? left : left < 0 ? soonest : std::min(soonest, left);
    }

    return soonest;
}

void capture_thumbnails::tick(uint64_t now_ns, damage_list& damage) {
    for (std::unique_ptr<thumbnail_card>& card : m_cards) {
        int32_t slide = slide_at(now_ns > card->shown_ns ? now_ns - card->shown_ns : 0);
        if (slide != card->slide) {
            damage_card(*card, damage);
            card->slide = slide;
            damage_card(*card, damage);
        }
    }

    if (m_hover >= 0 || m_pressed >= 0) {
        return;
    }

    bool erased = false;
    for (size_t i = m_cards.size(); i-- > 0;) {
        if (m_cards[i]->deadline_ns <= now_ns) {
            erase(i, damage);
            erased = true;
        }
    }

    if (erased) {
        lay_out(damage);
    }
}

void capture_thumbnails::bounds(uint32_t output, std::vector<damage_list::rect>& out) const {
    for (const std::unique_ptr<thumbnail_card>& card : m_cards) {
        if (card->output == output && card->picture) {
            out.push_back(picture_rect(*card));
        }
    }

    if (m_tip && m_tip_output == output) {
        out.push_back(m_tip_rect);
    }
}

void capture_thumbnails::draw(uint32_t output, stlxgfx_surface_t* back, int32_t ox, int32_t oy,
                              const damage_list::rect& clip) const {
    for (const std::unique_ptr<thumbnail_card>& card : m_cards) {
        if (card->output != output || !card->picture) {
            continue;
        }

        damage_list::rect at = picture_rect(*card);
        damage_list::rect area = damage_list::intersect(at, clip);
        if (area.w > 0 && area.h > 0) {
            stlxgfx_blit_alpha(back, area.x - ox, area.y - oy, card->picture->surface(), area.x - at.x,
                               area.y - at.y, static_cast<uint32_t>(area.w), static_cast<uint32_t>(area.h));
        }
    }

    damage_list::rect tip = damage_list::intersect(m_tip_rect, clip);
    if (m_tip && m_tip_output == output && tip.w > 0 && tip.h > 0) {
        stlxgfx_blit_alpha(back, tip.x - ox, tip.y - oy, m_tip, tip.x - m_tip_rect.x, tip.y - m_tip_rect.y,
                           static_cast<uint32_t>(tip.w), static_cast<uint32_t>(tip.h));
    }
}

/* Stacks each output's cards up from its bottom right corner, the newest lowest, remaking the glass of those moved */
void capture_thumbnails::lay_out(damage_list& damage) {
    for (size_t i = m_cards.size(); i-- > 0;) {
        thumbnail_card& card = *m_cards[i];
        int32_t bottom = card.place.y + card.place.h - CARD_MARGIN;
        for (size_t newer = i + 1; newer < m_cards.size(); newer++) {
            if (m_cards[newer]->output == card.output) {
                bottom = std::min(bottom, m_cards[newer]->box.y - CARD_MARGIN);
            }
        }

        int32_t h = card_height(card);
        damage_list::rect box = { card.place.x + card.place.w - CARD_MARGIN - CARD_WIDTH, bottom - h, CARD_WIDTH, h };
        bool is_moved = box.x != card.glass_box.x || box.y != card.glass_box.y || !card.picture;
        if (is_moved) {
            damage_card(card, damage);
            card.box = box;
            if (!make_picture(card)) {
                card.picture.reset();
            }

            damage_card(card, damage);
        }
    }

    show_tip(damage);
}

bool capture_thumbnails::make_picture(thumbnail_card& card) {
    const stlxgfx_surface_t* frost = m_frost_of ? m_frost_of(card.output) : nullptr;
    if (!frost) {
        return false;
    }

    auto picture = std::make_unique<panel_picture>();
    int32_t local_x = card.box.x - card.place.x;
    int32_t local_y = card.box.y - card.place.y;
    if (!picture->make(nullptr, popover_style(), card.box.w, card.box.h, local_x, local_y, frost)) {
        return false;
    }

    card.picture = std::move(picture);
    card.glass_box = card.box;
    paint(card);

    return true;
}

/* Paints a card whole: the glass, the image, opaque ones rounded in a ring and a window's on its own shadow, the
 * caption, and under the pointer the actions */
void capture_thumbnails::paint(thumbnail_card& card) {
    if (!card.picture) {
        return;
    }

    stlxgfx_surface_t* content = card.picture->content();
    stlxgfx_blit(content, 0, 0, card.picture->background(), 0, 0, content->width, content->height);

    stlxgfx_ctx_t ctx;
    stlxgfx_ctx_init(&ctx, content);
    damage_list::rect image = image_rect(card);
    uint32_t image_w = static_cast<uint32_t>(image.w);
    uint32_t image_h = static_cast<uint32_t>(image.h);
    if (card.keeps_alpha) {
        stlxgfx_blit_alpha(content, image.x, image.y, card.image, 0, 0, image_w, image_h);
    } else {
        stlxgfx_blit_rounded_alpha(content, image.x, image.y, card.image, 0, 0, image_w, image_h, IMAGE_RADIUS);
        stlxgfx_ctx_draw_rounded_rect(&ctx, image.x - 1, image.y - 1, image_w + 2, image_h + 2, IMAGE_RADIUS + 1,
                                      STLXTHEME_LINE);
    }

    int32_t caption_top = image.y + image.h;
    draw_line(content, STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, CARD_PADDING + CAPTION_LEFT, caption_top,
              CAPTION_HEIGHT, title_of(card), STLXTHEME_TEXT_SECONDARY);

    bool is_hovered = m_hover >= 0 && m_cards[static_cast<size_t>(m_hover)].get() == &card;
    if (is_hovered) {
        bool is_preview_lit = m_hover_part == part::preview;
        bool is_discard_lit = m_hover_part == part::discard;
        draw_action(&ctx, content, preview_rect(card), is_preview_lit,
                    is_preview_lit ? m_preview_glyph_lit : m_preview_glyph);
        draw_action(&ctx, content, discard_rect(card), is_discard_lit,
                    is_discard_lit ? m_discard_glyph_lit : m_discard_glyph);

        bool is_close_lit = m_hover_part == part::close;
        draw_action(&ctx, content, close_rect(card), is_close_lit, is_close_lit ? m_close_glyph_lit : m_close_glyph);
    }

    card.picture->finish_paint({ { 0, 0, card.box.w, card.box.h } });
}

void capture_thumbnails::damage_card(const thumbnail_card& card, damage_list& damage) const {
    if (card.picture) {
        damage_list::rect r = picture_rect(card);
        damage.add(r.x, r.y, r.w, r.h);
    }
}

/* The picture's rect on the desktop as it slides in, its shadow included */
damage_list::rect capture_thumbnails::picture_rect(const thumbnail_card& card) const {
    ui::rect box = card.picture->box();
    return { card.box.x + card.slide - box.x, card.box.y - box.y, static_cast<int32_t>(card.picture->surface()->width),
             static_cast<int32_t>(card.picture->surface()->height) };
}

int32_t capture_thumbnails::card_at(int32_t x, int32_t y) const {
    for (size_t i = m_cards.size(); i-- > 0;) {
        if (m_cards[i]->picture && ::contains(m_cards[i]->box, x, y)) {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

capture_thumbnails::part capture_thumbnails::part_at(const thumbnail_card& card, int32_t x, int32_t y) const {
    int32_t local_x = x - card.box.x;
    int32_t local_y = y - card.box.y;
    if (::contains(close_rect(card), local_x, local_y)) {
        return part::close;
    }

    if (::contains(preview_rect(card), local_x, local_y)) {
        return part::preview;
    }

    if (::contains(discard_rect(card), local_x, local_y)) {
        return part::discard;
    }

    return ::contains(image_rect(card), local_x, local_y) ? part::image : part::none;
}

/* The lit action's tooltip, above it and inside its output, remade when the action or its words change */
void capture_thumbnails::show_tip(damage_list& damage) {
    std::string words;
    damage_list::rect anchor = {};
    const thumbnail_card* card = m_hover >= 0 ? m_cards[static_cast<size_t>(m_hover)].get() : nullptr;
    bool has_tip = card && card->picture && (m_hover_part == part::preview || m_hover_part == part::discard);
    if (has_tip) {
        bool is_preview = m_hover_part == part::preview;
        words = is_preview ? PREVIEW_TIP : card->restores ? DISCARD_TIP : DISCARD_ONLY_TIP;
        damage_list::rect action = is_preview ? preview_rect(*card) : discard_rect(*card);
        anchor = { card->box.x + action.x, card->box.y + action.y, action.w, action.h };
    }

    stlxgfx_font* font = shell_font(STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_CAPTION_SIZE);
    if (m_tip) {
        damage.add(m_tip_rect.x, m_tip_rect.y, m_tip_rect.w, m_tip_rect.h);
        stlxgfx_destroy_surface(m_tip);
        m_tip = nullptr;
    }

    if (words.empty() || !font) {
        return;
    }

    int32_t w = stlxgfx_text_width(font, words.data(), words.size()) + 2 * TIP_PADDING_X;
    int32_t x = std::clamp(anchor.x + (anchor.w - w) / 2, card->place.x,
                           std::max(card->place.x, card->place.x + card->place.w - w));
    m_tip_rect = { x, anchor.y - TIP_GAP - TIP_HEIGHT, w, TIP_HEIGHT };
    m_tip_output = card->output;
    m_tip = stlxgfx_create_surface(static_cast<uint32_t>(w), TIP_HEIGHT, 32, 16, 8, 0);
    if (!m_tip) {
        return;
    }

    m_tip->flags = STLXGFX_SURFACE_STRAIGHT_ALPHA;
    stlxgfx_clear(m_tip, 0);
    int32_t local_x = m_tip_rect.x - card->place.x;
    int32_t local_y = m_tip_rect.y - card->place.y;
    const stlxgfx_surface_t* frost = m_frost_of ? m_frost_of(card->output) : nullptr;
    draw_chip_glass(m_tip, local_x, local_y, frost, { local_x, local_y, w, TIP_HEIGHT }, STLXTHEME_CONTROL_RADIUS);

    stlxgfx_font_metrics metrics;
    stlxgfx_font_metrics_get(font, &metrics);
    int32_t baseline = (TIP_HEIGHT + metrics.ascent - metrics.descent) / 2;
    stlxgfx_draw_text(m_tip, font, TIP_PADDING_X, baseline, words.data(), words.size(), STLXTHEME_TEXT);
    damage.add(m_tip_rect.x, m_tip_rect.y, m_tip_rect.w, m_tip_rect.h);
}

/* Takes a card down, fixing the hover and press that point past it */
void capture_thumbnails::erase(size_t index, damage_list& damage) {
    damage_card(*m_cards[index], damage);
    m_cards.erase(m_cards.begin() + static_cast<ptrdiff_t>(index));

    int32_t gone = static_cast<int32_t>(index);
    for (int32_t* held : { &m_hover, &m_pressed }) {
        *held = *held == gone ? -1 : *held > gone ? *held - 1 : *held;
    }

    if (m_hover < 0) {
        m_hover_part = part::none;
    }

    show_tip(damage);
}
