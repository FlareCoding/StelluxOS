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

/* One dragged file as a dock launcher weighs it: its path, whether it is a folder, and the program that opens files of
 * its type, empty for none, with that program's own space separated arguments, which come before the paths */
struct dock_file {
    std::string path;
    bool        is_folder = false;
    std::string opener;
    std::string opener_args;
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

    int32_t    press_x = 0;             /* where the press grabbed the item, which the picture lifts off and returns to */
    int32_t    press_y = 0;
    int32_t    x = 0;                   /* the pointer */
    int32_t    y = 0;
    uint16_t   buttons = 0;             /* held, as the newest pointer report gave them */
    uint8_t    modifiers = 0;
    bool       motion_pending = false;  /* a DROP_MOTION waits for the end of the wakeup */

    dm_window* target = nullptr;        /* the window under the pointer that got DROP_ENTER, or that took the drop */
    dnd_answer answer;
    uint32_t   action = 0;              /* what a release does now, 0 for nothing */

    /* The dock launcher under the pointer, which stlxdm answers itself, -1 for none, when resting there brings its
     * app's window forward, 0 for never, and whether its tile shows dark for the blink just before */
    int32_t    launcher = -1;
    uint64_t   spring_ns = 0;
    bool       is_blinking = false;

    std::vector<dock_file> files;       /* the dragged files, read once a launcher first weighs the drag */
    bool                   has_files = false;

    uint64_t   finish_deadline_ns = 0;  /* while dropped */
};

/* How a drag's picture moves apart from the pointer */
struct dnd_motion {
    enum class kind {
        none,       /* the picture rides the pointer */
        lift,       /* eases from its item to the pointer, its shadow fading in */
        settle,     /* fades where it was dropped, shrinking toward the pointer */
        fade,       /* fades where it is, for a cancel whose source window is gone */
        snap_back,  /* slides back to its item, its shadow fading last */
    };

    kind              type = kind::none;
    uint64_t          start_ns = 0;
    uint64_t          duration_ns = 0;
    damage_list::rect from;              /* the picture's rect as the motion starts */
    damage_list::rect to;                /* the item's place, where a snap-back ends */
    uint32_t          from_opacity = 0;  /* the picture's opacity as a settle, fade or snap-back starts */
    int32_t           pointer_x = 0;     /* where a settle shrinks toward */
    int32_t           pointer_y = 0;
};

/* A drag's picture at one moment: its rect, which its shadow follows at the same scale, and the opacities of both, 0
 * to 255 */
struct dnd_frame {
    damage_list::rect picture;
    uint32_t          opacity = 0;
    uint32_t          shadow_opacity = 0;
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

/* A lift from `item`, the picture's rect with its hotspot where the press grabbed it, over 120 ms */
dnd_motion lift_motion(const damage_list::rect& item, uint64_t now_ns);

/* A settle of the picture `shown` toward the release at (`x`, `y`), over 120 ms */
dnd_motion settle_motion(const dnd_frame& shown, int32_t x, int32_t y, uint64_t now_ns);

/* A snap-back of the picture `shown` to `item` in 160 ms plus 1 ms per 12 px it travels, at most 320 ms, or a fade
 * where it is over 120 ms when `item` is empty */
dnd_motion snap_back_motion(const dnd_frame& shown, const damage_list::rect& item, uint64_t now_ns);

/* Whether `motion` has run its course by `now_ns`, which a picture without motion always has */
bool has_dnd_motion_ended(const dnd_motion& motion, uint64_t now_ns);

/* The picture `motion` shows at `now_ns`. One riding the pointer, as a lift's end does, lies at `at_pointer` and shows
 * at `opacity`, its shadow too. */
dnd_frame step_dnd_motion(const dnd_motion& motion, uint64_t now_ns, const damage_list::rect& at_pointer,
                          uint32_t opacity);

/* Whether `program` is the terminal, whose launcher opens a dragged folder as its working folder */
bool is_terminal_program(const char* program);

/* What the dock launcher of `program`, labeled `label`, answers a drag of `files`: a copy when `program` opens every
 * file, or when it is the terminal and the drag is one folder, with words that say so, else a refusal */
dnd_answer launcher_answer(const std::vector<dock_file>& files, const char* program, const char* label);

#endif
