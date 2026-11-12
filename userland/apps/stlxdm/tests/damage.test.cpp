/* damage_list's splitting, run on the build machine: the rects it keeps never overlap and cover exactly the rects
 * added to it, unless it overflowed into a full repaint */

#include "damage.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

/* Random rects start on a grid that includes negative coordinates and are at most `RECT_MAX` wide and tall, empty
 * ones included */
constexpr int32_t GRID_FIRST = -8;
constexpr int32_t GRID_END = 56;
constexpr int32_t RECT_MAX = 32;
constexpr uint32_t RANDOM_ROUNDS = 1000;
constexpr uint32_t ADDS_PER_ROUND = 12;
constexpr uint32_t RANDOM_SEED = 0x2545F491u;
constexpr uint32_t MAX_REPORTS = 8;

static uint64_t g_checks;
static uint64_t g_failures;
static uint32_t g_random = RANDOM_SEED;

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

static bool holds_pixel(const damage_list::rect& r, int32_t x, int32_t y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Every kept rect holds pixels, and every grid pixel lies in at most one kept rect, and in one exactly when an added
 * rect holds it */
static void check_cover(const damage_list& damage, const std::vector<damage_list::rect>& added, const char* what) {
    bool is_exact = true;
    for (uint32_t i = 0; i < damage.count(); i++) {
        is_exact = is_exact && damage.at(i).w > 0 && damage.at(i).h > 0;
    }

    for (int32_t y = GRID_FIRST; y < GRID_END; y++) {
        for (int32_t x = GRID_FIRST; x < GRID_END; x++) {
            uint32_t kept = 0;
            for (uint32_t i = 0; i < damage.count(); i++) {
                kept += holds_pixel(damage.at(i), x, y) ? 1 : 0;
            }

            bool is_added = false;
            for (const damage_list::rect& r : added) {
                is_added = is_added || holds_pixel(r, x, y);
            }

            is_exact = is_exact && kept == (is_added ? 1u : 0u);
        }
    }

    check(is_exact, what);
}

static damage_list add_all(const std::vector<damage_list::rect>& rects) {
    damage_list damage;
    for (const damage_list::rect& r : rects) {
        damage.add(r.x, r.y, r.w, r.h);
    }

    return damage;
}

static void check_rule(const std::vector<damage_list::rect>& rects, uint32_t count, const char* what) {
    damage_list damage = add_all(rects);
    check(!damage.full() && damage.count() == count, what);
    check_cover(damage, rects, what);
}

static void check_overflow() {
    std::vector<damage_list::rect> apart;
    for (int32_t i = 0; i < static_cast<int32_t>(damage_list::MAX_RECTS); i++) {
        apart.push_back({ i * 4, 0, 2, 2 });
    }

    damage_list most = add_all(apart);
    check(!most.full() && most.count() == damage_list::MAX_RECTS, "as many rects as fit stay as added");

    most.add(apart.back().x, apart.back().y, 1, 1);
    check(!most.full(), "a rect another holds fits in a full list");

    most.add(apart.back().x + 4, 0, 2, 2);
    check(most.full(), "a rect past the last one that fits repaints fully");

    /* Across the first apart rect a rect splits into two parts, and across the second as well into three */
    std::vector<damage_list::rect> roomy(apart.begin(), apart.end() - 2);
    damage_list split = add_all(roomy);
    split.add(0, 1, 4, 2);
    check(!split.full() && split.count() == damage_list::MAX_RECTS, "a rect split into the room left fits");

    damage_list over = add_all(roomy);
    over.add(0, 1, 7, 2);
    check(over.full(), "a rect split past the room left repaints fully");
}

static int32_t random_below(int32_t bound) {
    g_random ^= g_random << 13;
    g_random ^= g_random >> 17;
    g_random ^= g_random << 5;
    return static_cast<int32_t>(g_random % static_cast<uint32_t>(bound));
}

static void check_random_rects() {
    for (uint32_t round = 0; round < RANDOM_ROUNDS; round++) {
        damage_list damage;
        std::vector<damage_list::rect> added;
        for (uint32_t i = 0; i < ADDS_PER_ROUND && !damage.full(); i++) {
            damage_list::rect r;
            r.x = GRID_FIRST + random_below(GRID_END - GRID_FIRST - RECT_MAX);
            r.y = GRID_FIRST + random_below(GRID_END - GRID_FIRST - RECT_MAX);
            r.w = random_below(RECT_MAX + 1);
            r.h = random_below(RECT_MAX + 1);

            damage.add(r.x, r.y, r.w, r.h);
            added.push_back(r);
            if (!damage.full()) {
                check_cover(damage, added, "random rects never overlap and cover what was added");
            }
        }
    }
}

/* A capture copies a region measured on one layout out of a frame of another, which clip cuts to the frame, as a 400
 * by 300 region kept at 1920x1080 meets a 1280x720 frame */
static void check_clip_to_frame() {
    damage_list::rect past = { 1400, 700, 400, 300 };
    check(!damage_list::clip(past, 1280, 720), "a region past the frame leaves nothing to copy");

    damage_list::rect across = { 1000, 500, 400, 300 };
    check(damage_list::clip(across, 1280, 720), "a region across the frame's corner keeps its inside");
    check(across.x == 1000 && across.y == 500 && across.w == 280 && across.h == 220, "the inside is the corner's part");

    damage_list::rect before = { -100, -50, 400, 300 };
    check(damage_list::clip(before, 1280, 720), "a region before the frame keeps its inside");
    check(before.x == 0 && before.y == 0 && before.w == 300 && before.h == 250, "nothing is read before the frame");

    damage_list::rect inside = { 10, 20, 400, 300 };
    check(damage_list::clip(inside, 1280, 720), "a region inside the frame stays whole");
    check(inside.x == 10 && inside.y == 20 && inside.w == 400 && inside.h == 300, "a whole region keeps its size");
}

int main() {
    check_rule({ { 0, 0, 10, 10 }, { 20, 0, 10, 10 } }, 2, "rects apart stay as added");
    check_rule({ { 0, 0, 10, 10 }, { 2, 2, 4, 4 } }, 1, "a rect another holds adds nothing");
    check_rule({ { 2, 2, 4, 4 }, { 8, 2, 4, 4 }, { 0, 0, 20, 10 } }, 1, "a rect drops the rects it holds");
    check_rule({ { 0, 0, 0, 10 }, { 0, 0, 10, -1 } }, 0, "empty rects add nothing");

    check_rule({ { 0, 0, 10, 10 }, { 4, 6, 10, 10 } }, 3, "rects overlapping at a corner keep their union");
    check_rule({ { 0, 0, 10, 10 }, { 3, 3, 4, 20 } }, 2, "a rect through an edge adds what sticks out");
    check_rule({ { 0, 0, 10, 10 }, { 10, 0, 10, 10 }, { 5, 0, 10, 10 } }, 2,
               "a rect two others cover together adds nothing");

    check_overflow();
    check_random_rects();
    check_clip_to_frame();

    if (g_failures != 0) {
        printf("damage: %llu of %llu checks failed\n", static_cast<unsigned long long>(g_failures),
               static_cast<unsigned long long>(g_checks));
        return 1;
    }

    printf("damage: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
