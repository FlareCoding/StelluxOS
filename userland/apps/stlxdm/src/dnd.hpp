#ifndef STLXDM_DND_HPP
#define STLXDM_DND_HPP

#include "clipboard.hpp"
#include "damage.hpp"

#include <stlxwin/proto.h>

#include <cstdint>
#include <string>
#include <vector>

struct dm_client;
struct dm_window;

/* Every action a drag may carry */
constexpr uint32_t DND_ACTIONS = SWP_DND_COPY | SWP_DND_MOVE | SWP_DND_LINK;

/* How long a window that took a drop has to finish it, after which it counts as having done nothing */
constexpr uint64_t DND_FINISH_TIMEOUT_NS = 5000000000ull;

/* The opacity of a drag's picture while nothing under it accepts, 56 percent */
constexpr uint32_t DND_REFUSED_OPACITY = 143;

/* A target's newest answer to the drag over it, which refuses until the target answers */
struct dnd_answer {
    uint32_t    actions = 0;
    uint32_t    preferred = 0;
    uint32_t    words_action = 0;
    std::string words;
};

/* What a drag's chip says: the glyph of the action a release does, or of a refusal, and the words beside it */
struct dnd_chip {
    enum class glyph { none, copy, move, link, refusal };

    glyph       mark = glyph::none;
    std::string words;

    bool operator==(const dnd_chip&) const = default;
};

/* One running drag, from its START_DRAG until its source hears it finished or cancelled */
struct dnd_session {
    enum class stage {
        dragging,  /* the picture follows the pointer */
        dropped,   /* released over a window that accepted, which reads the drag until it finishes */
    };

    stage          phase = stage::dragging;
    dm_client*     source = nullptr;  /* null once the source disconnected */
    uint32_t       source_win_id = 0;
    uint32_t       drag = 0;          /* the serial of the press it started from */
    uint32_t       button = 0;        /* the press's button, whose release drops */
    uint32_t       actions = 0;       /* SWP_DND_* the source allows */
    uint32_t       item_count = 1;
    clipboard_item item;              /* the representations, whose objects close with the session */

    /* The picture as premultiplied ARGB rows `picture_w` pixels apart, empty for none, and its pixel under the
     * pointer */
    std::vector<uint32_t> picture;
    uint32_t              picture_w = 0;
    uint32_t              picture_h = 0;
    int32_t               hot_x = 0;
    int32_t               hot_y = 0;

    int32_t    x = 0;                   /* the pointer */
    int32_t    y = 0;
    uint16_t   buttons = 0;             /* held, as the newest pointer report gave them */
    uint8_t    modifiers = 0;
    bool       motion_pending = false;  /* a DROP_MOTION waits for the end of the wakeup */

    dm_window* target = nullptr;        /* the window under the pointer that got DROP_ENTER, or that took the drop */
    dnd_answer answer;
    uint32_t   action = 0;              /* what a release does now, 0 for nothing */

    uint64_t   finish_deadline_ns = 0;  /* while dropped */
};

/* Whether `action` is exactly one known action */
bool is_one_dnd_action(uint32_t action);

/* The action a drop does: Ctrl Shift links, Ctrl copies and Shift moves when the source and the target both allow
 * it, and otherwise the target's preferred, else the first of move, copy and link they both allow, else none */
uint32_t choose_dnd_action(uint32_t source_actions, const dnd_answer& answer, uint8_t modifiers);

/* A DROP_STATUS as the session keeps it: unknown actions dropped, a preferred or words action that is not one action
 * as 0, a preferred one the target does not accept as 0, and the words up to their NUL without control characters */
dnd_answer read_drop_status(const swp_drop_status& status);

/* Reads a START_DRAG's representations, none for a drag only its own client takes, false for no known action or an
 * unknown one, no items, a picture neither 0 by 0 nor within SWP_DRAG_PICTURE_MAX, or an offer read_offer refuses */
bool read_start_drag(const swp_start_drag& start, std::vector<clipboard_rep>* reps);

/* The chip for `action` under the target's `answer`: the action's glyph with the target's words when they describe it,
 * else the action's name, the refusal glyph with a refusal's reason, and nothing for a refusal without one */
dnd_chip chip_for(uint32_t action, const dnd_answer& answer);

/* Where the chip's body of `width` by `height` goes for the pointer at (`x`, `y`) over `picture`, empty for none: right
 * of the pointer and under the picture, flipped left and above it at the edges of `output` */
damage_list::rect place_chip(int32_t x, int32_t y, const damage_list::rect& picture, int32_t width, int32_t height,
                             const damage_list::rect& output);

#endif
