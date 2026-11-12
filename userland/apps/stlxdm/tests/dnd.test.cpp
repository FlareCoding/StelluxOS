/* dnd, run on the build machine: the action a drop does under each modifier and answer, how a target's answer reads,
 * and which START_DRAGs keep the protocol's rules */

#include "dnd.hpp"
#include "shell/chord.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

constexpr uint32_t MAX_REPORTS = 8;

/* A bit no action uses */
constexpr uint32_t UNKNOWN_ACTION = 1u << 4;

/* A representation's bytes in the offers start_of builds */
constexpr uint64_t TEXT_LENGTH = 5;

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static dnd_answer answer_of(uint32_t actions, uint32_t preferred) {
    dnd_answer answer;
    answer.actions = actions;
    answer.preferred = preferred;

    return answer;
}

static void check_actions() {
    dnd_answer terminal = answer_of(SWP_DND_COPY, SWP_DND_COPY);
    dnd_answer folder = answer_of(DND_ACTIONS, SWP_DND_MOVE);

    check(choose_dnd_action(DND_ACTIONS, folder, 0) == SWP_DND_MOVE, "no modifier takes the target's preferred action");
    check(choose_dnd_action(DND_ACTIONS, folder, CHORD_MOD_CTRL) == SWP_DND_COPY, "Ctrl copies");
    check(choose_dnd_action(DND_ACTIONS, folder, CHORD_MOD_SHIFT) == SWP_DND_MOVE, "Shift moves");
    check(choose_dnd_action(DND_ACTIONS, folder, CHORD_MOD_CTRL | CHORD_MOD_SHIFT) == SWP_DND_LINK, "Ctrl Shift links");
    check(choose_dnd_action(DND_ACTIONS, folder, CHORD_MOD_ALT) == SWP_DND_MOVE, "Alt asks for no action");

    check(choose_dnd_action(DND_ACTIONS, terminal, CHORD_MOD_SHIFT) == SWP_DND_COPY,
          "Shift over a target that only copies still copies");
    check(choose_dnd_action(SWP_DND_COPY | SWP_DND_MOVE, folder, CHORD_MOD_CTRL | CHORD_MOD_SHIFT) == SWP_DND_MOVE,
          "a link the source refuses gives way to the target's preferred action");
    check(choose_dnd_action(DND_ACTIONS, answer_of(SWP_DND_COPY | SWP_DND_LINK, 0), 0) == SWP_DND_COPY,
          "without a preferred action the first of move, copy and link allowed is taken");
    check(choose_dnd_action(SWP_DND_LINK, folder, 0) == SWP_DND_LINK,
          "a preferred action the source refuses gives way to what both allow");

    check(choose_dnd_action(SWP_DND_MOVE, terminal, CHORD_MOD_CTRL) == 0, "nothing both allow does nothing");
    check(choose_dnd_action(DND_ACTIONS, answer_of(0, 0), 0) == 0, "a refusal does nothing");
}

static swp_drop_status status_of(uint32_t actions, uint32_t preferred, uint32_t words_action, const char* words) {
    swp_drop_status status = {};
    status.actions = actions;
    status.preferred = preferred;
    status.words_action = words_action;
    snprintf(status.words, sizeof(status.words), "%s", words);

    return status;
}

static void check_answers() {
    dnd_answer answer = read_drop_status(status_of(SWP_DND_COPY | UNKNOWN_ACTION, SWP_DND_COPY, SWP_DND_COPY,
                                                   "Paste path"));
    check(answer.actions == SWP_DND_COPY, "unknown actions leave an answer");
    check(answer.preferred == SWP_DND_COPY && answer.words_action == SWP_DND_COPY && answer.words == "Paste path",
          "an answer keeps its preferred action and its words");

    check(read_drop_status(status_of(SWP_DND_COPY, SWP_DND_MOVE, 0, "")).preferred == 0,
          "a preferred action the target does not accept reads as none");
    check(read_drop_status(status_of(DND_ACTIONS, SWP_DND_COPY | SWP_DND_MOVE, 0, "")).preferred == 0,
          "a preferred value naming two actions reads as none");
    check(read_drop_status(status_of(0, 0, SWP_DND_MOVE | SWP_DND_LINK, "Move")).words_action == 0,
          "words for two actions describe none");
    check(read_drop_status(status_of(0, 0, 0, "Read\x01only\x7f")).words == "Readonly",
          "control characters leave the words");

    swp_drop_status unterminated = status_of(SWP_DND_COPY, 0, 0, "");
    memset(unterminated.words, 'a', sizeof(unterminated.words));
    check(read_drop_status(unterminated).words.size() == SWP_DROP_WORDS_MAX - 1,
          "words without their NUL keep all but the field's last byte");
}

static swp_start_drag start_of(uint32_t actions, uint32_t item_count, uint32_t width, uint32_t height,
                               uint32_t types) {
    swp_start_drag start = {};
    start.actions = actions;
    start.item_count = item_count;
    start.picture_w = width;
    start.picture_h = height;
    for (uint32_t i = 0; i < types && i < SWP_OFFER_TYPES_MAX; i++) {
        swp_offer_type& type = start.offer.types[start.offer.count++];
        snprintf(type.mime, sizeof(type.mime), "text/x-test-%u", i);
        type.len = TEXT_LENGTH;
    }

    return start;
}

static void check_starts() {
    std::vector<clipboard_rep> reps;
    check(read_start_drag(start_of(SWP_DND_COPY | SWP_DND_MOVE, 1, 64, 32, 2), &reps) && reps.size() == 2,
          "a drag with a picture and two representations reads");
    check(read_start_drag(start_of(SWP_DND_COPY, 3, 0, 0, 1), &reps), "a drag without a picture reads");
    check(read_start_drag(start_of(SWP_DND_LINK, 1, SWP_DRAG_PICTURE_MAX, SWP_DRAG_PICTURE_MAX, 4), &reps),
          "a picture at the largest size reads");
    check(read_start_drag(start_of(SWP_DND_MOVE, 1, 0, 0, 0), &reps) && reps.empty(),
          "a drag of no representation, which only its own client takes, reads");

    check(!read_start_drag(start_of(0, 1, 0, 0, 1), &reps), "a drag without an action is malformed");
    check(!read_start_drag(start_of(SWP_DND_COPY | UNKNOWN_ACTION, 1, 0, 0, 1), &reps),
          "an unknown action is malformed");
    check(!read_start_drag(start_of(SWP_DND_COPY, 0, 0, 0, 1), &reps), "an item count of 0 is malformed");
    check(!read_start_drag(start_of(SWP_DND_COPY, 1, 16, 0, 1), &reps), "a picture without rows is malformed");
    check(!read_start_drag(start_of(SWP_DND_COPY, 1, SWP_DRAG_PICTURE_MAX + 1, 8, 1), &reps),
          "a picture past the largest width is malformed");

    swp_start_drag five = start_of(SWP_DND_COPY, 1, 0, 0, SWP_OFFER_TYPES_MAX);
    five.offer.count = SWP_OFFER_TYPES_MAX + 1;
    check(!read_start_drag(five, &reps), "an offer past the most representations is malformed");
}

int main() {
    check_actions();
    check_answers();
    check_starts();

    if (g_failures != 0) {
        printf("dnd: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("dnd: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
