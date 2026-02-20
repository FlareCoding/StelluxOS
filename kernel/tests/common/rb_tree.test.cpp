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

TEST(rb_tree, empty_tree_operations) {
    test_tree tree;

    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(tree.size(), static_cast<size_t>(0));
    EXPECT_NULL(tree.min());
    EXPECT_NULL(tree.max());
    EXPECT_TRUE(tree.validate());

    test_item probe{42, {}};
    EXPECT_NULL(tree.find(probe));
    EXPECT_NULL(tree.lower_bound(probe));
    EXPECT_NULL(tree.upper_bound(probe));
}

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

TEST(rb_tree, find_missing_key) {
    test_item a{10, {}};
    test_item b{20, {}};
    test_item c{30, {}};
    test_tree tree;

    ASSERT_TRUE(tree.insert(&a));
    ASSERT_TRUE(tree.insert(&b));
    ASSERT_TRUE(tree.insert(&c));

    test_item probe{25, {}};
    EXPECT_NULL(tree.find(probe));

    test_item probe2{0, {}};
    EXPECT_NULL(tree.find(probe2));

    test_item probe3{99, {}};
    EXPECT_NULL(tree.find(probe3));
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

TEST(rb_tree, insert_descending) {
    constexpr size_t N = 64;
    test_item items[N];
    for (size_t i = 0; i < N; i++) {
        items[i].key = N - 1 - i;
        items[i].link = {};
    }

    test_tree tree;
    for (size_t i = 0; i < N; i++) {
        ASSERT_TRUE(tree.insert(&items[i]));
    }

    EXPECT_EQ(tree.size(), N);
    EXPECT_TRUE(tree.validate());

    auto* min = tree.min();
    auto* max = tree.max();
    ASSERT_NOT_NULL(min);
    ASSERT_NOT_NULL(max);
    EXPECT_EQ(min->key, static_cast<uint64_t>(0));
    EXPECT_EQ(max->key, static_cast<uint64_t>(N - 1));
}

TEST(rb_tree, lower_bound) {
    test_item items[5];
    uint64_t keys[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; i++) {
        items[i].key = keys[i];
        items[i].link = {};
    }

    test_tree tree;
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(tree.insert(&items[i]));
    }

    test_item p25{25, {}};
    auto* lb25 = tree.lower_bound(p25);
    ASSERT_NOT_NULL(lb25);
    EXPECT_EQ(lb25->key, static_cast<uint64_t>(30));

    test_item p10{10, {}};
    auto* lb10 = tree.lower_bound(p10);
    ASSERT_NOT_NULL(lb10);
    EXPECT_EQ(lb10->key, static_cast<uint64_t>(10));

    test_item p5{5, {}};
    auto* lb5 = tree.lower_bound(p5);
    ASSERT_NOT_NULL(lb5);
    EXPECT_EQ(lb5->key, static_cast<uint64_t>(10));

    test_item p55{55, {}};
    EXPECT_NULL(tree.lower_bound(p55));
}

TEST(rb_tree, upper_bound) {
    test_item items[5];
    uint64_t keys[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; i++) {
        items[i].key = keys[i];
        items[i].link = {};
    }

    test_tree tree;
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(tree.insert(&items[i]));
    }

    test_item p20{20, {}};
    auto* ub20 = tree.upper_bound(p20);
    ASSERT_NOT_NULL(ub20);
    EXPECT_EQ(ub20->key, static_cast<uint64_t>(30));

    test_item p50{50, {}};
    EXPECT_NULL(tree.upper_bound(p50));

    test_item p0{0, {}};
    auto* ub0 = tree.upper_bound(p0);
    ASSERT_NOT_NULL(ub0);
    EXPECT_EQ(ub0->key, static_cast<uint64_t>(10));
}

TEST(rb_tree, next_prev_traversal) {
    constexpr size_t N = 32;
    test_item items[N];
    for (size_t i = 0; i < N; i++) {
        items[i].key = i * 3;
        items[i].link = {};
    }

    test_tree tree;
    for (size_t i = 0; i < N; i++) {
        ASSERT_TRUE(tree.insert(&items[i]));
    }

    // Walk forward from min
    auto* cur = tree.min();
    ASSERT_NOT_NULL(cur);
    size_t forward_count = 1;
    while (auto* n = tree.next(*cur)) {
        EXPECT_GT(n->key, cur->key);
        cur = n;
        forward_count++;
    }
    EXPECT_EQ(forward_count, N);

    // Walk backward from max
    cur = tree.max();
    ASSERT_NOT_NULL(cur);
    size_t backward_count = 1;
    while (auto* p = tree.prev(*cur)) {
        EXPECT_LT(p->key, cur->key);
        cur = p;
        backward_count++;
    }
    EXPECT_EQ(backward_count, N);
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

TEST(rb_tree, remove_all_items) {
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

    for (size_t i = 0; i < N; i++) {
        tree.remove(items[i]);
        EXPECT_EQ(tree.size(), N - 1 - i);
        EXPECT_TRUE(tree.validate());
    }

    EXPECT_TRUE(tree.empty());
    EXPECT_NULL(tree.min());
    EXPECT_NULL(tree.max());
}

TEST(rb_tree, remove_internal_node) {
    // Insert keys so that key 20 has two children
    test_item items[7];
    uint64_t keys[] = {20, 10, 30, 5, 15, 25, 35};
    for (int i = 0; i < 7; i++) {
        items[i].key = keys[i];
        items[i].link = {};
    }

    test_tree tree;
    for (int i = 0; i < 7; i++) {
        ASSERT_TRUE(tree.insert(&items[i]));
    }

    // Remove internal node with two children (key 20 or 10 or 30)
    tree.remove(items[0]); // key 20
    EXPECT_EQ(tree.size(), static_cast<size_t>(6));
    EXPECT_NULL(tree.find(items[0]));
    EXPECT_TRUE(tree.validate());

    // All other keys must still be findable
    for (int i = 1; i < 7; i++) {
        EXPECT_NOT_NULL(tree.find(items[i]));
    }
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

TEST(rb_tree, validate_with_diagnostic) {
    constexpr size_t N = 16;
    test_item items[N];
    for (size_t i = 0; i < N; i++) {
        items[i].key = i;
        items[i].link = {};
    }

    test_tree tree;
    for (size_t i = 0; i < N; i++) {
        ASSERT_TRUE(tree.insert(&items[i]));
    }

    const char* err = nullptr;
    EXPECT_TRUE(tree.validate(err));
    EXPECT_NULL(err);
}

TEST(rb_tree, stress_insert_remove) {
    constexpr size_t N = 512;
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
    ASSERT_TRUE(tree.validate());

    // Remove every other item (odd indices)
    for (size_t i = 1; i < N; i += 2) {
        tree.remove(items[i]);
    }
    EXPECT_EQ(tree.size(), N / 2);
    ASSERT_TRUE(tree.validate());

    // Verify remaining items are findable
    for (size_t i = 0; i < N; i += 2) {
        EXPECT_NOT_NULL(tree.find(items[i]));
    }
    // Verify removed items are gone
    for (size_t i = 1; i < N; i += 2) {
        EXPECT_NULL(tree.find(items[i]));
    }

    // Remove the rest
    for (size_t i = 0; i < N; i += 2) {
        tree.remove(items[i]);
    }
    EXPECT_TRUE(tree.empty());
    EXPECT_TRUE(tree.validate());
}
