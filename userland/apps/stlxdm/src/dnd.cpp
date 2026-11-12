#include "dnd.hpp"
#include "shell/chord.hpp"

#include <algorithm>
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
