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

constexpr uint64_t MS = 1000000ull;

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

static dnd_answer worded(uint32_t actions, uint32_t words_action, const char* words) {
    dnd_answer answer = answer_of(actions, 0);
    answer.words_action = words_action;
    answer.words = words;

    return answer;
}

static void check_chips() {
    dnd_chip chip = chip_for(SWP_DND_COPY, worded(SWP_DND_COPY, SWP_DND_COPY, "Paste path"));
    check(chip.mark == dnd_chip::glyph::copy && chip.words == "Paste path", "words for the chosen action show");

    chip = chip_for(SWP_DND_MOVE, worded(DND_ACTIONS, SWP_DND_COPY, "Copy to wallpapers"));
    check(chip.mark == dnd_chip::glyph::move && chip.words == "Move",
          "words for another action give way to the chosen action's name");
    check(chip_for(SWP_DND_LINK, answer_of(DND_ACTIONS, 0)).words == "Link", "an action without words shows its name");

    chip = chip_for(0, worded(0, 0, "Can't move a folder into itself"));
    check(chip.mark == dnd_chip::glyph::refusal && chip.words == "Can't move a folder into itself",
          "a refusal with a reason shows the refusal glyph and the reason");
    check(chip_for(0, answer_of(0, 0)) == dnd_chip(), "a refusal without a reason shows no chip");
    check(chip_for(0, worded(SWP_DND_MOVE, SWP_DND_MOVE, "Move here")) == dnd_chip(),
          "nothing the source allows shows no chip");
}

static bool is_rect(const damage_list::rect& r, int32_t x, int32_t y, int32_t w, int32_t h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

static void check_chip_places() {
    damage_list::rect output = { 0, 0, 1920, 1080 };
    damage_list::rect picture = { 90, 90, 200, 40 };

    check(is_rect(place_chip(100, 100, picture, 120, 28, output), 118, 136, 120, 28),
          "the chip sits right of the pointer and under the picture");
    check(place_chip(100, 100, { 90, 95, 30, 10 }, 120, 28, output).y == 124,
          "the chip keeps clear of the pointer under a short picture");
    check(is_rect(place_chip(100, 100, {}, 120, 28, output), 118, 124, 120, 28),
          "without a picture the chip rides the pointer alone");

    check(place_chip(100, 1060, { 90, 1050, 200, 40 }, 120, 28, output).y == 1016,
          "at the bottom edge the chip moves above the picture");
    check(place_chip(1880, 100, picture, 120, 28, output).x == 1742,
          "at the right edge the chip moves left of the pointer");

    damage_list::rect second = { 1920, 0, 1280, 1024 };
    check(place_chip(1930, 100, { 1920, 90, 200, 40 }, 120, 28, second).x == 1948,
          "an output right of the main one places the chip in its own pixels");
}

static bool is_between(uint32_t value, uint32_t low, uint32_t high) {
    return value >= low && value <= high;
}

static void check_lift() {
    damage_list::rect at_pointer = { 400, 300, 160, 40 };
    dnd_motion lift = lift_motion({ 100, 200, 160, 40 }, 0);

    dnd_frame start = step_dnd_motion(lift, 0, at_pointer, 255);
    check(is_rect(start.picture, 100, 200, 160, 40) && start.shadow_opacity == 0,
          "a lift starts exactly over its item, without a shadow");

    dnd_frame third = step_dnd_motion(lift, 40 * MS, at_pointer, 255);
    check(third.picture.x >= 305 && third.picture.x <= 315, "a third of the way in, a lift has eased about 70 percent");
    check(is_between(third.shadow_opacity, 170, 190), "its shadow fades in along the same curve");

    dnd_frame end = step_dnd_motion(lift, 120 * MS, { 420, 310, 160, 40 }, 143);
    check(is_rect(end.picture, 420, 310, 160, 40) && end.opacity == 143 && end.shadow_opacity == 143,
          "a lift ends on the pointer where it moved, at the opacity the drag shows");
    check(!has_dnd_motion_ended(lift, 119 * MS) && has_dnd_motion_ended(lift, 120 * MS), "a lift takes 120 ms");
}

static void check_settle() {
    dnd_frame shown = { { 400, 300, 200, 100 }, 255, 255 };
    dnd_motion settle = settle_motion(shown, 450, 320, 0);

    check(is_between(step_dnd_motion(settle, 40 * MS, {}, 0).opacity, 70, 80),
          "40 ms into a settle the picture shows about 30 percent");
    check(is_between(step_dnd_motion(settle, 80 * MS, {}, 0).opacity, 7, 12), "and about 4 percent at 80 ms");

    dnd_frame end = step_dnd_motion(settle, 120 * MS, {}, 0);
    check(end.opacity == 0 && end.shadow_opacity == 0 && has_dnd_motion_ended(settle, 120 * MS),
          "a settle fades out completely in 120 ms");
    check(is_rect(end.picture, 402, 301, 192, 96), "it shrinks 4 percent toward where it was released");

    dnd_frame faded = { { 400, 300, 200, 100 }, 143, 143 };
    check(step_dnd_motion(settle_motion(faded, 450, 320, 0), 0, {}, 0).opacity == 143,
          "a settle starts at the opacity the picture showed");
}

static void check_snap_back() {
    damage_list::rect item = { 100, 300, 200, 40 };
    dnd_frame shown = { { 772, 300, 200, 40 }, 143, 143 };
    dnd_motion snap = snap_back_motion(shown, item, 0);
    check(snap.type == dnd_motion::kind::snap_back && snap.duration_ns == 216 * MS,
          "a snap-back of 672 px takes 160 ms plus 56 ms");
    check(snap_back_motion(shown, { 772, 300, 200, 40 }, 0).duration_ns == 160 * MS, "one that stays put takes 160 ms");
    check(snap_back_motion(shown, { -9000, 300, 200, 40 }, 0).duration_ns == 320 * MS, "and none takes over 320 ms");

    dnd_frame start = step_dnd_motion(snap, 0, {}, 0);
    check(is_rect(start.picture, 772, 300, 200, 40) && start.opacity == 143 && start.shadow_opacity == 143,
          "a snap-back starts where the picture was let go");
    check(is_between(step_dnd_motion(snap, 186 * MS, {}, 0).shadow_opacity, 100, 130),
          "its shadow is half gone 30 ms before it lands");

    dnd_frame end = step_dnd_motion(snap, 216 * MS, {}, 0);
    check(is_rect(end.picture, 100, 300, 200, 40) && end.opacity == 255 && end.shadow_opacity == 0,
          "it lands whole on its item without a shadow");

    dnd_motion fade = snap_back_motion(shown, {}, 0);
    dnd_frame faded = step_dnd_motion(fade, 120 * MS, {}, 0);
    check(fade.type == dnd_motion::kind::fade && is_rect(faded.picture, 772, 300, 200, 40) && faded.opacity == 0,
          "without an item to return to, the picture fades out where it is");
}

static dock_file file_of(const char* path, bool is_folder, const char* opener) {
    dock_file file;
    file.path = path;
    file.is_folder = is_folder;
    file.opener = opener;

    return file;
}

static void check_launchers() {
    const char* sheen = "/usr/bin/sheen";
    std::vector<dock_file> note = { file_of("/tmp/release notes.md", false, sheen) };
    dnd_answer one = launcher_answer(note, sheen, "Sheen");
    check(one.actions == SWP_DND_COPY && one.preferred == SWP_DND_COPY && one.words_action == SWP_DND_COPY,
          "a launcher whose program opens the file takes it as a copy");
    check(one.words == "Open in Sheen", "and names its program");

    std::vector<dock_file> three = { file_of("/a.c", false, sheen), file_of("/b.h", false, sheen),
                                     file_of("/c.md", false, sheen) };
    check(launcher_answer(three, sheen, "Sheen").words == "Open 3 files in Sheen", "several files are counted");

    std::vector<dock_file> mixed = { file_of("/a.c", false, sheen), file_of("/b.png", false, "/bin/viewer") };
    check(launcher_answer(mixed, sheen, "Sheen").actions == 0, "a file its program does not open refuses them all");

    std::vector<dock_file> folder = { file_of("/tmp/wallpapers", true, "") };
    dnd_answer terminal = launcher_answer(folder, "/bin/stlxterm", "Terminal");
    check(terminal.actions == SWP_DND_COPY && terminal.words == "Open Terminal in wallpapers",
          "a folder opens the terminal in it");
    check(launcher_answer({ file_of("/", true, "") }, "/bin/stlxterm", "Terminal").words == "Open Terminal in /",
          "the root keeps its slash as its name");
    check(launcher_answer(folder, sheen, "Sheen").actions == 0, "a folder refuses a launcher that does not open it");

    std::vector<dock_file> folders = { file_of("/a", true, ""), file_of("/b", true, "") };
    check(launcher_answer(folders, "/bin/stlxterm", "Terminal").actions == 0, "two folders refuse the terminal");
    check(launcher_answer(note, "/bin/stlxterm", "Terminal").actions == 0, "a file the terminal does not open refuses");
    check(launcher_answer(note, "", "Empty").actions == 0 && launcher_answer({}, sheen, "Sheen").actions == 0,
          "a launcher without a program, and a drag without files, refuse");
}

int main() {
    check_actions();
    check_answers();
    check_starts();
    check_chips();
    check_chip_places();

    check_lift();
    check_settle();
    check_snap_back();
    check_launchers();

    if (g_failures != 0) {
        printf("dnd: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("dnd: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
