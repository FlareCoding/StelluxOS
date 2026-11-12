#ifndef STLXDM_CAPTURE_THUMBNAILS_HPP
#define STLXDM_CAPTURE_THUMBNAILS_HPP

#include "capture.hpp"
#include "clipboard.hpp"
#include "damage.hpp"
#include "shell/panel_picture.hpp"

#include <stlxgfx/surface.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/* How long a notice about captures shows unless it says otherwise */
constexpr uint64_t NOTICE_SHOWN_NS = 3000000000ull;

enum class thumbnail_action { none, preview, discard, retry };

/* What a click on a thumbnail asks of the server, and the capture it names */
struct thumbnail_click {
    thumbnail_action action = thumbnail_action::none;
    uint64_t         job = 0;       /* the capture's encoder job */
    uint64_t         entry = 0;     /* its history entry, 0 while it encodes */
};

struct thumbnail_card;

/* The cards of the latest captures at the bottom right of their outputs, the newest lowest and three at most, and a
 * notice about captures. Each card stays 6 seconds, all held under the pointer, which shows its actions. */
class capture_thumbnails {
public:
    capture_thumbnails();
    ~capture_thumbnails();

    capture_thumbnails(const capture_thumbnails&) = delete;
    capture_thumbnails& operator=(const capture_thumbnails&) = delete;

    void set_frost_lookup(frost_lookup lookup) { m_frost_of = std::move(lookup); }

    /* Shows a card for the capture `job` encodes, shrunk at once from `pixels`, which the card reads but the encoder
     * keeps until it hands them back */
    void add(uint64_t job, capture_kind kind, const stlxgfx_surface_t* pixels, bool keeps_alpha, uint32_t output,
             const damage_list::rect& place, uint64_t now_ns, damage_list& damage);

    /* The capture `job` reached the clipboard as `entry`, and its card keeps `pixels` for its preview. False when no
     * card waits for it, the pixels left to the caller. */
    bool encoded(uint64_t job, uint64_t entry, stlxgfx_surface_t* pixels);

    /* What the card of `job` previews, its pixels valid until the card goes. False without the card. */
    bool preview_of(uint64_t job, preview_source* out) const;

    /* The Discard of the card of `job`, as its own button asks it, which keeps a capture still encoding off the
     * clipboard. No action without the card. */
    thumbnail_click discard_of(uint64_t job);

    /* The kind the card of `job` names, such as "Region", or null without the card */
    const char* kind_name_of(uint64_t job) const;

    /* The capture `job` found no memory on the clipboard, and its card keeps `pixels` and offers Try Again until it
     * closes. False when no card waits for it, the pixels left to the caller. */
    bool failed(uint64_t job, stlxgfx_surface_t* pixels, damage_list& damage);

    /* Try Again on the card of `job`: its pixels go back to the encoder, which the card reads again meanwhile. Null
     * without a failed card. */
    stlxgfx_surface_t* retry(uint64_t job, bool* keeps_alpha, uint64_t now_ns, damage_list& damage);

    /* Whether a Discard took the card of `job` before it encoded, which keeps the capture off the clipboard */
    bool take_dropped(uint64_t job);

    void remove(uint64_t job, damage_list& damage);
    void clear(damage_list& damage);

    /* Shows `chip`, which it takes, at the top middle of output `output` at `place` on the desktop for `shown_ns` */
    void notify(stlxgfx_surface_t* chip, uint32_t output, const damage_list::rect& place, uint64_t now_ns,
                damage_list& damage, uint64_t shown_ns = NOTICE_SHOWN_NS);

    /* Each card says whether its capture is still the clipboard, and goes once its entry leaves the history */
    void follow_clipboard(const clipboard_history& history, damage_list& damage);

    bool contains(int32_t x, int32_t y) const;
    bool holds_pointer() const { return m_pressed >= 0; }
    void pointer_move(int32_t x, int32_t y, uint64_t now_ns, damage_list& damage);
    void press(int32_t x, int32_t y, uint64_t now_ns, damage_list& damage);
    thumbnail_click release(int32_t x, int32_t y, uint64_t now_ns, damage_list& damage);

    /* How long until a card's time runs out, -1 with none counting */
    int64_t timeout_ns(uint64_t now_ns) const;
    void tick(uint64_t now_ns, damage_list& damage);

    /* The rects of `output` the cards and the tooltip draw in, on the desktop */
    void bounds(uint32_t output, std::vector<damage_list::rect>& out) const;

    /* Blends the cards of `output` and the tooltip into `back`, whose first pixel shows the desktop point
     * (`ox`, `oy`), inside `clip` on the desktop */
    void draw(uint32_t output, stlxgfx_surface_t* back, int32_t ox, int32_t oy, const damage_list::rect& clip) const;

private:
    enum class part { none, image, preview, discard, close, retry };

    void lay_out(damage_list& damage);
    bool make_picture(thumbnail_card& card);
    void paint(thumbnail_card& card);
    void damage_card(const thumbnail_card& card, damage_list& damage) const;
    damage_list::rect picture_rect(const thumbnail_card& card) const;
    int32_t card_at(int32_t x, int32_t y) const;
    part part_at(const thumbnail_card& card, int32_t x, int32_t y) const;
    void show_tip(damage_list& damage);
    void erase(size_t index, damage_list& damage);

    std::vector<std::unique_ptr<thumbnail_card>> m_cards;  /* oldest first */
    std::vector<uint64_t>                        m_dropped;
    frost_lookup                                 m_frost_of;

    int32_t m_hover = -1;
    part    m_hover_part = part::none;
    int32_t m_pressed = -1;
    part    m_pressed_part = part::none;

    stlxgfx_surface_t* m_preview_glyph = nullptr;
    stlxgfx_surface_t* m_preview_glyph_lit = nullptr;
    stlxgfx_surface_t* m_discard_glyph = nullptr;
    stlxgfx_surface_t* m_discard_glyph_lit = nullptr;
    stlxgfx_surface_t* m_close_glyph = nullptr;
    stlxgfx_surface_t* m_close_glyph_lit = nullptr;
    stlxgfx_surface_t* m_warning_glyph = nullptr;
    stlxgfx_surface_t* m_tip = nullptr;
    damage_list::rect  m_tip_rect = {};
    uint32_t           m_tip_output = 0;

    stlxgfx_surface_t* m_notice = nullptr;
    damage_list::rect  m_notice_rect = {};
    uint32_t           m_notice_output = 0;
    uint64_t           m_notice_until_ns = 0;
};

#endif
