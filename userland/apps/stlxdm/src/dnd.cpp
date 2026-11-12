#include "dnd.hpp"
#include "easing.hpp"
#include "shell/chord.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

/* The control characters a target's words lose */
constexpr unsigned char FIRST_PRINTABLE = 0x20;
constexpr unsigned char DELETE_CHARACTER = 0x7F;

/* The order a drop falls back through when neither a modifier nor the target picks its action */
constexpr uint32_t FALLBACK_ORDER[] = { SWP_DND_MOVE, SWP_DND_COPY, SWP_DND_LINK };

/* The chip sits this far right of the pointer, this far under the picture and at least this far below the pointer */
constexpr int32_t CHIP_RIGHT_OF_POINTER = 18;
constexpr int32_t CHIP_UNDER_PICTURE = 6;
constexpr int32_t CHIP_BELOW_POINTER = 24;

/* A lift, a settle and a fade take this, and a snap-back the base plus 1 ms per `SNAP_BACK_PX_PER_MS` pixels it
 * travels, at most its most, with its shadow fading over its last `SNAP_BACK_SHADOW_NS` */
constexpr uint64_t LIFT_NS = 120000000ull;
constexpr uint64_t SETTLE_NS = 120000000ull;
constexpr uint64_t SNAP_BACK_BASE_NS = 160000000ull;
constexpr uint64_t SNAP_BACK_MAX_NS = 320000000ull;
constexpr uint64_t SNAP_BACK_SHADOW_NS = 60000000ull;
constexpr uint64_t NS_PER_MS = 1000000ull;
constexpr double SNAP_BACK_PX_PER_MS = 12.0;

/* A settle shrinks the picture by 4 percent, in `PROGRESS_ONE` units */
constexpr int64_t SETTLE_SHRINK = PROGRESS_ONE * 4 / 100;
constexpr int32_t FULL_OPACITY = 255;

bool is_one_dnd_action(uint32_t action) {
    return action != 0 && (action & (action - 1)) == 0 && (action & ~DND_ACTIONS) == 0;
}

/* The action the held modifiers ask for, 0 for none */
static uint32_t asked_action(uint8_t modifiers) {
    bool has_ctrl = (modifiers & CHORD_MOD_CTRL) != 0;
    bool has_shift = (modifiers & CHORD_MOD_SHIFT) != 0;
    if (has_ctrl && has_shift) {
        return SWP_DND_LINK;
    }

    if (has_ctrl) {
        return SWP_DND_COPY;
    }

    return has_shift ? SWP_DND_MOVE : 0;
}

uint32_t choose_dnd_action(uint32_t source_actions, const dnd_answer& answer, uint8_t modifiers) {
    uint32_t allowed = source_actions & answer.actions & DND_ACTIONS;
    if (allowed == 0) {
        return 0;
    }

    uint32_t asked = asked_action(modifiers);
    if ((asked & allowed) != 0) {
        return asked;
    }

    if (is_one_dnd_action(answer.preferred) && (answer.preferred & allowed) != 0) {
        return answer.preferred;
    }

    for (uint32_t action : FALLBACK_ORDER) {
        if ((action & allowed) != 0) {
            return action;
        }
    }

    return 0;
}

dnd_answer read_drop_status(const swp_drop_status& status) {
    dnd_answer answer;
    answer.actions = status.actions & DND_ACTIONS;
    if (is_one_dnd_action(status.preferred) && (status.preferred & answer.actions) != 0) {
        answer.preferred = status.preferred;
    }

    if (is_one_dnd_action(status.words_action)) {
        answer.words_action = status.words_action;
    }

    size_t len = strnlen(status.words, sizeof(status.words) - 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = static_cast<unsigned char>(status.words[i]);
        if (c >= FIRST_PRINTABLE && c != DELETE_CHARACTER) {
            answer.words += static_cast<char>(c);
        }
    }

    return answer;
}

bool read_start_drag(const swp_start_drag& start, std::vector<clipboard_rep>* reps) {
    bool has_known_action = (start.actions & DND_ACTIONS) != 0 && (start.actions & ~DND_ACTIONS) == 0;
    bool has_no_picture = start.picture_w == 0 && start.picture_h == 0;
    bool is_width_inside = start.picture_w >= 1 && start.picture_w <= SWP_DRAG_PICTURE_MAX;
    bool is_height_inside = start.picture_h >= 1 && start.picture_h <= SWP_DRAG_PICTURE_MAX;
    bool is_picture_valid = has_no_picture || (is_width_inside && is_height_inside);
    if (!has_known_action || start.item_count == 0 || !is_picture_valid) {
        return false;
    }

    return read_offer(start.offer, reps);
}

static const char* action_name(uint32_t action) {
    switch (action) {
    case SWP_DND_COPY: return "Copy";
    case SWP_DND_MOVE: return "Move";
    case SWP_DND_LINK: return "Link";
    default:           return "";
    }
}

static dnd_chip::glyph action_glyph(uint32_t action) {
    switch (action) {
    case SWP_DND_COPY: return dnd_chip::glyph::copy;
    case SWP_DND_MOVE: return dnd_chip::glyph::move;
    case SWP_DND_LINK: return dnd_chip::glyph::link;
    default:           return dnd_chip::glyph::none;
    }
}

dnd_chip chip_for(uint32_t action, const dnd_answer& answer) {
    dnd_chip chip;
    if (action != 0) {
        bool has_own_words = answer.words_action == action && !answer.words.empty();
        chip.mark = action_glyph(action);
        chip.words = has_own_words ? answer.words : action_name(action);
        return chip;
    }

    bool has_reason = answer.actions == 0 && answer.words_action == 0 && !answer.words.empty();
    if (has_reason) {
        chip.mark = dnd_chip::glyph::refusal;
        chip.words = answer.words;
    }

    return chip;
}

damage_list::rect place_chip(int32_t x, int32_t y, const damage_list::rect& picture, int32_t width, int32_t height,
                             const damage_list::rect& output) {
    bool has_picture = picture.w > 0 && picture.h > 0;
    int32_t below = y + CHIP_BELOW_POINTER;
    if (has_picture) {
        below = std::max(below, picture.y + picture.h + CHIP_UNDER_PICTURE);
    }

    damage_list::rect chip = { x + CHIP_RIGHT_OF_POINTER, below, width, height };
    if (chip.y + chip.h > output.y + output.h) {
        int32_t above = has_picture ? picture.y : y;
        chip.y = above - CHIP_UNDER_PICTURE - height;
    }

    if (chip.x + chip.w > output.x + output.w) {
        chip.x = x - CHIP_RIGHT_OF_POINTER - width;
    }

    return chip;
}

dnd_motion lift_motion(const damage_list::rect& item, uint64_t now_ns) {
    dnd_motion motion;
    motion.type = dnd_motion::kind::lift;
    motion.start_ns = now_ns;
    motion.duration_ns = LIFT_NS;
    motion.from = item;

    return motion;
}

dnd_motion settle_motion(const dnd_frame& shown, int32_t x, int32_t y, uint64_t now_ns) {
    dnd_motion motion;
    motion.type = dnd_motion::kind::settle;
    motion.start_ns = now_ns;
    motion.duration_ns = SETTLE_NS;
    motion.from = shown.picture;

    motion.from_opacity = shown.opacity;
    motion.pointer_x = x;
    motion.pointer_y = y;
    return motion;
}

dnd_motion snap_back_motion(const dnd_frame& shown, const damage_list::rect& item, uint64_t now_ns) {
    dnd_motion motion;
    motion.start_ns = now_ns;
    motion.from = shown.picture;
    motion.from_opacity = shown.opacity;
    if (item.w <= 0 || item.h <= 0) {
        motion.type = dnd_motion::kind::fade;
        motion.duration_ns = SETTLE_NS;
        return motion;
    }

    double dx = static_cast<double>(item.x - shown.picture.x);
    double dy = static_cast<double>(item.y - shown.picture.y);
    uint64_t travel_ns = static_cast<uint64_t>(std::lround(std::hypot(dx, dy) / SNAP_BACK_PX_PER_MS * NS_PER_MS));

    motion.type = dnd_motion::kind::snap_back;
    motion.duration_ns = std::min(SNAP_BACK_BASE_NS + travel_ns, SNAP_BACK_MAX_NS);
    motion.to = item;
    return motion;
}

bool has_dnd_motion_ended(const dnd_motion& motion, uint64_t now_ns) {
    return motion.type == dnd_motion::kind::none || now_ns >= motion.start_ns + motion.duration_ns;
}

static uint32_t scale_opacity(uint32_t opacity, int64_t share) {
    return static_cast<uint32_t>(static_cast<int64_t>(opacity) * share >> PROGRESS_BITS);
}

static int32_t scale_length(int32_t length, int64_t scale) {
    return static_cast<int32_t>((length * scale + PROGRESS_ONE / 2) >> PROGRESS_BITS);
}

/* `r` scaled by `scale` in `PROGRESS_ONE` units about the point (`x`, `y`), which stays where it is */
static damage_list::rect scaled_about(const damage_list::rect& r, int32_t x, int32_t y, int64_t scale) {
    return { x + scale_length(r.x - x, scale), y + scale_length(r.y - y, scale), scale_length(r.w, scale),
             scale_length(r.h, scale) };
}

static damage_list::rect interpolate_rect(const damage_list::rect& from, const damage_list::rect& to, int64_t progress) {
    return { interpolate(from.x, to.x, progress), interpolate(from.y, to.y, progress),
             interpolate(from.w, to.w, progress), interpolate(from.h, to.h, progress) };
}

/* A lift and a snap-back ease their place, a settle and a fade keep (1 - t)^3 of their opacity, the eased curve's
 * remainder, and a snap-back's shadow fades out linearly over its last stretch so the picture lands as its item */
dnd_frame step_dnd_motion(const dnd_motion& motion, uint64_t now_ns, const damage_list::rect& at_pointer,
                          uint32_t opacity) {
    uint64_t elapsed = now_ns > motion.start_ns ? std::min(now_ns - motion.start_ns, motion.duration_ns) : 0;
    int64_t progress = motion.type == dnd_motion::kind::none ? PROGRESS_ONE : ease_out(elapsed, motion.duration_ns);
    dnd_frame frame = { at_pointer, opacity, opacity };
    switch (motion.type) {
    case dnd_motion::kind::none:
        break;
    case dnd_motion::kind::lift:
        frame.picture = interpolate_rect(motion.from, at_pointer, progress);
        frame.shadow_opacity = scale_opacity(opacity, progress);
        break;
    case dnd_motion::kind::settle:
    case dnd_motion::kind::fade: {
        bool shrinks = motion.type == dnd_motion::kind::settle;
        int64_t scale = PROGRESS_ONE - (shrinks ? SETTLE_SHRINK * progress >> PROGRESS_BITS : 0);
        frame.picture = scaled_about(motion.from, motion.pointer_x, motion.pointer_y, scale);
        frame.opacity = scale_opacity(motion.from_opacity, PROGRESS_ONE - progress);
        frame.shadow_opacity = frame.opacity;
        break;
    }
    case dnd_motion::kind::snap_back: {
        frame.picture = interpolate_rect(motion.from, motion.to, progress);
        frame.opacity = static_cast<uint32_t>(interpolate(static_cast<int32_t>(motion.from_opacity), FULL_OPACITY,
                                                          progress));
        uint64_t left = motion.duration_ns - elapsed;
        int64_t shadow_share = left >= SNAP_BACK_SHADOW_NS ? PROGRESS_ONE
                                                           : static_cast<int64_t>(left * PROGRESS_ONE / SNAP_BACK_SHADOW_NS);
        frame.shadow_opacity = scale_opacity(frame.opacity, shadow_share);
        break;
    }
    }

    return frame;
}
