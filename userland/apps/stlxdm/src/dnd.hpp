#ifndef STLXDM_DND_HPP
#define STLXDM_DND_HPP

#include "clipboard.hpp"

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

/* A target's newest answer to the drag over it, which refuses until the target answers */
struct dnd_answer {
    uint32_t    actions = 0;
    uint32_t    preferred = 0;
    uint32_t    words_action = 0;
    std::string words;
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

#endif
