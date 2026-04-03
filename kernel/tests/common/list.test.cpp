#define STLX_TEST_TIER TIER_DS

#include "stlx_unit_test.h"
#include "common/list.h"

TEST_SUITE(list);

namespace {

struct test_item {
    uint64_t value;
    list::node link;
};

using test_list = list::head<test_item, &test_item::link>;

} // namespace

TEST(list, empty_list_operations) {
    test_list lst;
    lst.init();

    EXPECT_TRUE(lst.empty());
    EXPECT_EQ(lst.size(), static_cast<size_t>(0));
    EXPECT_NULL(lst.front());
    EXPECT_NULL(lst.back());
    EXPECT_NULL(lst.pop_front());
    EXPECT_NULL(lst.pop_back());
}

TEST(list, push_back_single) {
    test_item item{42, {}};
    test_list lst;
    lst.init();

    lst.push_back(&item);

    EXPECT_FALSE(lst.empty());
    EXPECT_EQ(lst.size(), static_cast<size_t>(1));
    ASSERT_NOT_NULL(lst.front());
    EXPECT_EQ(lst.front()->value, static_cast<uint64_t>(42));
    EXPECT_EQ(lst.front(), lst.back());
}

TEST(list, push_front_single) {
    test_item item{99, {}};
    test_list lst;
    lst.init();

    lst.push_front(&item);

    EXPECT_FALSE(lst.empty());
    EXPECT_EQ(lst.size(), static_cast<size_t>(1));
    ASSERT_NOT_NULL(lst.front());
    EXPECT_EQ(lst.front()->value, static_cast<uint64_t>(99));
    EXPECT_EQ(lst.front(), lst.back());
}

TEST(list, push_back_order) {
    test_item a{1, {}}, b{2, {}}, c{3, {}};
    test_list lst;
    lst.init();

    lst.push_back(&a);
    lst.push_back(&b);
    lst.push_back(&c);

    EXPECT_EQ(lst.size(), static_cast<size_t>(3));
    ASSERT_NOT_NULL(lst.front());
    ASSERT_NOT_NULL(lst.back());
    EXPECT_EQ(lst.front()->value, static_cast<uint64_t>(1));
    EXPECT_EQ(lst.back()->value, static_cast<uint64_t>(3));
}

TEST(list, push_front_order) {
    test_item a{1, {}}, b{2, {}}, c{3, {}};
    test_list lst;
    lst.init();

    lst.push_front(&a);
    lst.push_front(&b);
    lst.push_front(&c);

    EXPECT_EQ(lst.size(), static_cast<size_t>(3));
    ASSERT_NOT_NULL(lst.front());
    ASSERT_NOT_NULL(lst.back());
    EXPECT_EQ(lst.front()->value, static_cast<uint64_t>(3));
    EXPECT_EQ(lst.back()->value, static_cast<uint64_t>(1));
}

TEST(list, pop_front_fifo) {
    test_item a{10, {}}, b{20, {}}, c{30, {}};
    test_list lst;
    lst.init();

    lst.push_back(&a);
    lst.push_back(&b);
    lst.push_back(&c);

    auto* first = lst.pop_front();
    ASSERT_NOT_NULL(first);
    EXPECT_EQ(first->value, static_cast<uint64_t>(10));
    EXPECT_EQ(lst.size(), static_cast<size_t>(2));

    auto* second = lst.pop_front();
    ASSERT_NOT_NULL(second);
    EXPECT_EQ(second->value, static_cast<uint64_t>(20));
    EXPECT_EQ(lst.size(), static_cast<size_t>(1));

    auto* third = lst.pop_front();
    ASSERT_NOT_NULL(third);
    EXPECT_EQ(third->value, static_cast<uint64_t>(30));
    EXPECT_EQ(lst.size(), static_cast<size_t>(0));

    EXPECT_TRUE(lst.empty());
    EXPECT_NULL(lst.pop_front());
}

TEST(list, pop_back_lifo) {
    test_item a{10, {}}, b{20, {}}, c{30, {}};
    test_list lst;
    lst.init();

    lst.push_back(&a);
    lst.push_back(&b);
    lst.push_back(&c);

    auto* last = lst.pop_back();
    ASSERT_NOT_NULL(last);
    EXPECT_EQ(last->value, static_cast<uint64_t>(30));
    EXPECT_EQ(lst.size(), static_cast<size_t>(2));

    auto* second = lst.pop_back();
    ASSERT_NOT_NULL(second);
    EXPECT_EQ(second->value, static_cast<uint64_t>(20));

    auto* first = lst.pop_back();
    ASSERT_NOT_NULL(first);
    EXPECT_EQ(first->value, static_cast<uint64_t>(10));

    EXPECT_TRUE(lst.empty());
}

TEST(list, remove_middle) {
    test_item a{1, {}}, b{2, {}}, c{3, {}};
    test_list lst;
    lst.init();

    lst.push_back(&a);
    lst.push_back(&b);
    lst.push_back(&c);

    lst.remove(&b);

    EXPECT_EQ(lst.size(), static_cast<size_t>(2));
    ASSERT_NOT_NULL(lst.front());
    ASSERT_NOT_NULL(lst.back());
    EXPECT_EQ(lst.front()->value, static_cast<uint64_t>(1));
    EXPECT_EQ(lst.back()->value, static_cast<uint64_t>(3));
}

TEST(list, remove_front) {
    test_item a{1, {}}, b{2, {}}, c{3, {}};
    test_list lst;
    lst.init();

    lst.push_back(&a);
    lst.push_back(&b);
    lst.push_back(&c);

    lst.remove(&a);

    EXPECT_EQ(lst.size(), static_cast<size_t>(2));
    ASSERT_NOT_NULL(lst.front());
    EXPECT_EQ(lst.front()->value, static_cast<uint64_t>(2));
}

TEST(list, remove_back) {
    test_item a{1, {}}, b{2, {}}, c{3, {}};
    test_list lst;
    lst.init();

    lst.push_back(&a);
    lst.push_back(&b);
    lst.push_back(&c);

    lst.remove(&c);

    EXPECT_EQ(lst.size(), static_cast<size_t>(2));
    ASSERT_NOT_NULL(lst.back());
    EXPECT_EQ(lst.back()->value, static_cast<uint64_t>(2));
}

TEST(list, remove_only_item) {
    test_item a{42, {}};
    test_list lst;
    lst.init();

    lst.push_back(&a);
    lst.remove(&a);

    EXPECT_TRUE(lst.empty());
    EXPECT_EQ(lst.size(), static_cast<size_t>(0));
    EXPECT_NULL(lst.front());
    EXPECT_NULL(lst.back());
}

TEST(list, remove_clears_node_links) {
    test_item a{1, {}}, b{2, {}};
    test_list lst;
    lst.init();

    lst.push_back(&a);
    lst.push_back(&b);

    lst.remove(&a);
    EXPECT_NULL(a.link.prev);
    EXPECT_NULL(a.link.next);
}

TEST(list, remove_all_items) {
    constexpr size_t N = 32;
    test_item items[N];
    for (size_t i = 0; i < N; i++) {
        items[i].value = i;
        items[i].link = {};
    }

    test_list lst;
    lst.init();
    for (size_t i = 0; i < N; i++) {
        lst.push_back(&items[i]);
    }

    for (size_t i = 0; i < N; i++) {
        lst.remove(&items[i]);
        EXPECT_EQ(lst.size(), N - 1 - i);
    }

    EXPECT_TRUE(lst.empty());
}

TEST(list, iterator_forward) {
    constexpr size_t N = 16;
    test_item items[N];
    for (size_t i = 0; i < N; i++) {
        items[i].value = i * 10;
        items[i].link = {};
    }

    test_list lst;
    lst.init();
    for (size_t i = 0; i < N; i++) {
        lst.push_back(&items[i]);
    }

    uint64_t expected = 0;
    size_t count = 0;
    for (auto& entry : lst) {
        EXPECT_EQ(entry.value, expected);
        expected += 10;
        count++;
    }
    EXPECT_EQ(count, N);
}

TEST(list, push_back_pop_front_interleaved) {
    test_item a{1, {}}, b{2, {}}, c{3, {}}, d{4, {}};
    test_list lst;
    lst.init();

    lst.push_back(&a);
    lst.push_back(&b);
    auto* x = lst.pop_front();
    ASSERT_NOT_NULL(x);
    EXPECT_EQ(x->value, static_cast<uint64_t>(1));

    lst.push_back(&c);
    lst.push_back(&d);
    EXPECT_EQ(lst.size(), static_cast<size_t>(3));

    x = lst.pop_front();
    ASSERT_NOT_NULL(x);
    EXPECT_EQ(x->value, static_cast<uint64_t>(2));
    x = lst.pop_front();
    ASSERT_NOT_NULL(x);
    EXPECT_EQ(x->value, static_cast<uint64_t>(3));
    x = lst.pop_front();
    ASSERT_NOT_NULL(x);
    EXPECT_EQ(x->value, static_cast<uint64_t>(4));
    EXPECT_TRUE(lst.empty());
}

TEST(list, reinsert_after_remove) {
    test_item a{1, {}}, b{2, {}};
    test_list lst;
    lst.init();

    lst.push_back(&a);
    lst.push_back(&b);
    lst.remove(&a);

    EXPECT_EQ(lst.size(), static_cast<size_t>(1));
    EXPECT_EQ(lst.front()->value, static_cast<uint64_t>(2));

    lst.push_back(&a);
    EXPECT_EQ(lst.size(), static_cast<size_t>(2));
    EXPECT_EQ(lst.back()->value, static_cast<uint64_t>(1));
}

TEST(list, reinsert_after_pop) {
    test_item a{10, {}}, b{20, {}};
    test_list lst;
    lst.init();

    lst.push_back(&a);
    lst.push_back(&b);

    auto* popped = lst.pop_front();
    ASSERT_NOT_NULL(popped);
    EXPECT_EQ(popped->value, static_cast<uint64_t>(10));

    lst.push_back(popped);
    EXPECT_EQ(lst.size(), static_cast<size_t>(2));
    EXPECT_EQ(lst.front()->value, static_cast<uint64_t>(20));
    EXPECT_EQ(lst.back()->value, static_cast<uint64_t>(10));
}

TEST(list, mixed_push_front_back) {
    test_item a{1, {}}, b{2, {}}, c{3, {}}, d{4, {}};
    test_list lst;
    lst.init();

    lst.push_back(&b);
    lst.push_front(&a);
    lst.push_back(&c);
    lst.push_front(&d);

    // Expected order: d(4), a(1), b(2), c(3)
    EXPECT_EQ(lst.size(), static_cast<size_t>(4));
    auto* x = lst.pop_front();
    ASSERT_NOT_NULL(x);
    EXPECT_EQ(x->value, static_cast<uint64_t>(4));
    x = lst.pop_front();
    ASSERT_NOT_NULL(x);
    EXPECT_EQ(x->value, static_cast<uint64_t>(1));
    x = lst.pop_front();
    ASSERT_NOT_NULL(x);
    EXPECT_EQ(x->value, static_cast<uint64_t>(2));
    x = lst.pop_front();
    ASSERT_NOT_NULL(x);
    EXPECT_EQ(x->value, static_cast<uint64_t>(3));
    EXPECT_TRUE(lst.empty());
}

TEST(list, stress_fifo) {
    constexpr size_t N = 512;
    test_item items[N];
    for (size_t i = 0; i < N; i++) {
        items[i].value = i;
        items[i].link = {};
    }

    test_list lst;
    lst.init();

    for (size_t i = 0; i < N; i++) {
        lst.push_back(&items[i]);
    }
    EXPECT_EQ(lst.size(), N);

    for (size_t i = 0; i < N; i++) {
        auto* item = lst.pop_front();
        ASSERT_NOT_NULL(item);
        EXPECT_EQ(item->value, static_cast<uint64_t>(i));
    }
    EXPECT_TRUE(lst.empty());
}

TEST(list, stress_remove_alternating) {
    constexpr size_t N = 256;
    test_item items[N];
    for (size_t i = 0; i < N; i++) {
        items[i].value = i;
        items[i].link = {};
    }

    test_list lst;
    lst.init();
    for (size_t i = 0; i < N; i++) {
        lst.push_back(&items[i]);
    }

    // Remove odd indices
    for (size_t i = 1; i < N; i += 2) {
        lst.remove(&items[i]);
    }
    EXPECT_EQ(lst.size(), N / 2);

    // Verify remaining items are even-indexed, in order
    size_t expected = 0;
    for (auto& entry : lst) {
        EXPECT_EQ(entry.value, static_cast<uint64_t>(expected));
        expected += 2;
    }

    // Remove the rest
    for (size_t i = 0; i < N; i += 2) {
        lst.remove(&items[i]);
    }
    EXPECT_TRUE(lst.empty());
}

TEST(list, multiple_node_fields) {
    struct dual_item {
        uint64_t id;
        list::node link_a;
        list::node link_b;
    };

    using list_a = list::head<dual_item, &dual_item::link_a>;
    using list_b = list::head<dual_item, &dual_item::link_b>;

    dual_item x{1, {}, {}}, y{2, {}, {}}, z{3, {}, {}};

    list_a la;
    list_b lb;
    la.init();
    lb.init();

    la.push_back(&x);
    la.push_back(&y);
    la.push_back(&z);

    lb.push_back(&z);
    lb.push_back(&x);

    EXPECT_EQ(la.size(), static_cast<size_t>(3));
    EXPECT_EQ(lb.size(), static_cast<size_t>(2));

    EXPECT_EQ(la.front()->id, static_cast<uint64_t>(1));
    EXPECT_EQ(lb.front()->id, static_cast<uint64_t>(3));

    la.remove(&y);
    EXPECT_EQ(la.size(), static_cast<size_t>(2));
    EXPECT_EQ(lb.size(), static_cast<size_t>(2));
}
