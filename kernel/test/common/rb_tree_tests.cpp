#include "test/framework/test_framework.h"
#include "test/framework/test_utils.h"
#include "common/rb_tree.h"

namespace {

struct rb_item {
    uint64_t key;
    rbt::node link;
};

struct rb_item_cmp {
    bool operator()(const rb_item& a, const rb_item& b) const {
        return a.key < b.key;
    }
};

using rb_tree = rbt::tree<rb_item, &rb_item::link, rb_item_cmp>;

rb_item make_probe(uint64_t key) {
    return rb_item{.key = key, .link = {}};
}

size_t count_present(const bool* present, size_t n) {
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (present[i]) {
            count++;
        }
    }
    return count;
}

} // namespace

STLX_TEST_SUITE(core_rb_tree, test::phase::early);

STLX_TEST(core_rb_tree, insert_find_and_iteration_order) {
    rb_tree tree;
    rb_item items[] = {
        {.key = 7, .link = {}},
        {.key = 3, .link = {}},
        {.key = 9, .link = {}},
        {.key = 1, .link = {}},
        {.key = 8, .link = {}},
        {.key = 2, .link = {}},
        {.key = 5, .link = {}},
        {.key = 6, .link = {}},
        {.key = 4, .link = {}},
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        STLX_ASSERT_TRUE(ctx, tree.insert(&items[i]));
        STLX_ASSERT_TRUE(ctx, tree.validate());
    }

    STLX_ASSERT_EQ(ctx, tree.size(), static_cast<size_t>(9));

    for (uint64_t key = 1; key <= 9; key++) {
        rb_item probe = make_probe(key);
        rb_item* found = tree.find(probe);
        STLX_ASSERT_NOT_NULL(ctx, found);
        STLX_ASSERT_EQ(ctx, found->key, key);
    }

    uint64_t expected = 1;
    for (auto it = tree.begin(); it != tree.end(); ++it) {
        STLX_ASSERT_EQ(ctx, it->key, expected);
        expected++;
    }
    STLX_ASSERT_EQ(ctx, expected, static_cast<uint64_t>(10));
    STLX_ASSERT_TRUE(ctx, tree.validate());
}

STLX_TEST(core_rb_tree, duplicate_keys_are_rejected) {
    rb_tree tree;

    rb_item a{.key = 42, .link = {}};
    rb_item b{.key = 42, .link = {}};

    STLX_ASSERT_TRUE(ctx, tree.insert(&a));
    STLX_ASSERT_FALSE(ctx, tree.insert(&b));
    STLX_ASSERT_EQ(ctx, tree.size(), static_cast<size_t>(1));
    STLX_ASSERT_TRUE(ctx, tree.validate());
}

STLX_TEST(core_rb_tree, remove_and_bounds_queries) {
    rb_tree tree;
    rb_item items[16] = {};

    for (size_t i = 0; i < 16; i++) {
        items[i].key = i;
        STLX_ASSERT_TRUE(ctx, tree.insert(&items[i]));
    }
    STLX_ASSERT_TRUE(ctx, tree.validate());

    // Remove odd keys.
    for (size_t i = 1; i < 16; i += 2) {
        tree.remove(items[i]);
        STLX_ASSERT_TRUE(ctx, tree.validate());
    }

    STLX_ASSERT_EQ(ctx, tree.size(), static_cast<size_t>(8));

    for (uint64_t key = 0; key < 16; key++) {
        rb_item probe = make_probe(key);
        rb_item* found = tree.find(probe);
        if ((key & 1) == 0) {
            STLX_ASSERT_NOT_NULL(ctx, found);
            STLX_ASSERT_EQ(ctx, found->key, key);
        } else {
            STLX_ASSERT_NULL(ctx, found);
        }
    }

    rb_item probe7 = make_probe(7);
    rb_item* lb7 = tree.lower_bound(probe7);
    STLX_ASSERT_NOT_NULL(ctx, lb7);
    STLX_ASSERT_EQ(ctx, lb7->key, static_cast<uint64_t>(8));

    rb_item probe8 = make_probe(8);
    rb_item* ub8 = tree.upper_bound(probe8);
    STLX_ASSERT_NOT_NULL(ctx, ub8);
    STLX_ASSERT_EQ(ctx, ub8->key, static_cast<uint64_t>(10));

    rb_item* at6 = tree.find(make_probe(6));
    STLX_ASSERT_NOT_NULL(ctx, at6);
    rb_item* next = tree.next(*at6);
    rb_item* prev = tree.prev(*at6);
    STLX_ASSERT_NOT_NULL(ctx, next);
    STLX_ASSERT_NOT_NULL(ctx, prev);
    STLX_ASSERT_EQ(ctx, next->key, static_cast<uint64_t>(8));
    STLX_ASSERT_EQ(ctx, prev->key, static_cast<uint64_t>(4));
}

STLX_TEST(core_rb_tree, randomized_insert_and_find_preserve_invariants) {
    constexpr size_t KEY_SPACE = 64;
    constexpr size_t OPS = 1000;

    rb_tree tree;
    rb_item pool[KEY_SPACE] = {};
    bool present[KEY_SPACE] = {};

    for (size_t i = 0; i < KEY_SPACE; i++) {
        pool[i].key = i;
        present[i] = false;
    }

    for (size_t op = 0; op < OPS; op++) {
        uint32_t which = test::rand_u32(ctx) % 2;
        uint32_t key = test::rand_u32(ctx) % KEY_SPACE;

        if (which == 0) {
            if (present[key]) {
                // Intrusive nodes already linked in the tree must not be reinserted.
                rb_item probe = make_probe(key);
                rb_item* found = tree.find(probe);
                STLX_EXPECT_NOT_NULL(ctx, found);
            } else {
                bool inserted = tree.insert(&pool[key]);
                STLX_EXPECT_TRUE(ctx, inserted);
                present[key] = true;
            }
        } else {
            rb_item probe = make_probe(key);
            rb_item* found = tree.find(probe);
            if (present[key]) {
                STLX_EXPECT_NOT_NULL(ctx, found);
            } else {
                STLX_EXPECT_NULL(ctx, found);
            }
        }

        STLX_ASSERT_TRUE(ctx, tree.validate());
    }

    STLX_ASSERT_EQ(ctx, tree.size(), count_present(present, KEY_SPACE));
}
