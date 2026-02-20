#define STLX_TEST_TIER TIER_DS

#include "stlx_unit_test.h"
#include "common/rb_tree.h"

TEST_SUITE(rb_tree);

namespace {

struct test_item {
    uint64_t key;
    rbt::node link;
};

struct test_cmp {
    bool operator()(const test_item& a, const test_item& b) const {
        return a.key < b.key;
    }
};

using test_tree = rbt::tree<test_item, &test_item::link, test_cmp>;

} // namespace

TEST(rb_tree, insert_single) {
    test_item item{42, {}};
    test_tree tree;

    ASSERT_TRUE(tree.insert(&item));
    EXPECT_EQ(tree.size(), static_cast<size_t>(1));
    EXPECT_NOT_NULL(tree.find(item));
    EXPECT_TRUE(tree.validate());
}

TEST(rb_tree, insert_duplicate_rejected) {
    test_item a{10, {}};
    test_item b{10, {}};
    test_tree tree;

    ASSERT_TRUE(tree.insert(&a));
    EXPECT_FALSE(tree.insert(&b));
    EXPECT_EQ(tree.size(), static_cast<size_t>(1));
}

TEST(rb_tree, insert_ascending) {
    constexpr size_t N = 64;
    test_item items[N];
    for (size_t i = 0; i < N; i++) {
        items[i].key = i;
        items[i].link = {};
    }

    test_tree tree;
    for (size_t i = 0; i < N; i++) {
        ASSERT_TRUE(tree.insert(&items[i]));
    }

    EXPECT_EQ(tree.size(), N);
    EXPECT_TRUE(tree.validate());

    for (size_t i = 0; i < N; i++) {
        EXPECT_NOT_NULL(tree.find(items[i]));
    }
}

TEST(rb_tree, remove_and_validate) {
    constexpr size_t N = 32;
    test_item items[N];
    for (size_t i = 0; i < N; i++) {
        items[i].key = i;
        items[i].link = {};
    }

    test_tree tree;
    for (size_t i = 0; i < N; i++) {
        ASSERT_TRUE(tree.insert(&items[i]));
    }

    tree.remove(items[0]);
    EXPECT_EQ(tree.size(), N - 1);
    EXPECT_NULL(tree.find(items[0]));
    EXPECT_TRUE(tree.validate());
}

TEST(rb_tree, min_max) {
    test_item a{5, {}};
    test_item b{3, {}};
    test_item c{8, {}};
    test_item d{1, {}};
    test_item e{9, {}};
    test_tree tree;

    ASSERT_TRUE(tree.insert(&a));
    ASSERT_TRUE(tree.insert(&b));
    ASSERT_TRUE(tree.insert(&c));
    ASSERT_TRUE(tree.insert(&d));
    ASSERT_TRUE(tree.insert(&e));

    auto* min = tree.min();
    auto* max = tree.max();
    ASSERT_NOT_NULL(min);
    ASSERT_NOT_NULL(max);
    EXPECT_EQ(min->key, static_cast<uint64_t>(1));
    EXPECT_EQ(max->key, static_cast<uint64_t>(9));
}

TEST(rb_tree, iterator_order) {
    constexpr size_t N = 16;
    test_item items[N];
    for (size_t i = 0; i < N; i++) {
        items[i].key = N - 1 - i;
        items[i].link = {};
    }

    test_tree tree;
    for (size_t i = 0; i < N; i++) {
        ASSERT_TRUE(tree.insert(&items[i]));
    }

    uint64_t prev_key = 0;
    bool first = true;
    for (auto& entry : tree) {
        if (!first) {
            EXPECT_GT(entry.key, prev_key);
        }
        prev_key = entry.key;
        first = false;
    }
}
