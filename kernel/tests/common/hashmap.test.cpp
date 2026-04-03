#define STLX_TEST_TIER TIER_DS

#include "stlx_unit_test.h"
#include "common/hashmap.h"
#include "common/hash.h"
#include "common/string.h"

TEST_SUITE(hashmap);

namespace {

struct test_item {
    uint64_t id;
    uint64_t value;
    hashmap::node link;
};

struct test_key_ops {
    using key_type = uint64_t;
    static key_type key_of(const test_item& e) { return e.id; }
    static uint64_t hash(const key_type& k) { return hash::u64(k); }
    static bool equal(const key_type& a, const key_type& b) { return a == b; }
};

using test_map = hashmap::map<test_item, &test_item::link, test_key_ops>;

constexpr uint32_t TEST_BUCKETS = 16;

// Force all items into bucket 0 by making hash return 0.
struct collide_key_ops {
    using key_type = uint64_t;
    static key_type key_of(const test_item& e) { return e.id; }
    static uint64_t hash(const key_type&) { return 0; }
    static bool equal(const key_type& a, const key_type& b) { return a == b; }
};

using collide_map = hashmap::map<test_item, &test_item::link, collide_key_ops>;

struct str_item {
    const char* name;
    uint64_t value;
    hashmap::node link;
};

struct str_key_ops {
    using key_type = const char*;
    static key_type key_of(const str_item& e) { return e.name; }
    static uint64_t hash(const key_type& k) { return hash::string(k); }
    static bool equal(const key_type& a, const key_type& b) {
        return string::strcmp(a, b) == 0;
    }
};

using str_map = hashmap::map<str_item, &str_item::link, str_key_ops>;

struct composite_key {
    uint64_t parent_id;
    const char* name;
};

struct comp_item {
    uint64_t parent_id;
    const char* name;
    uint64_t value;
    hashmap::node link;
};

struct comp_key_ops {
    using key_type = composite_key;
    static key_type key_of(const comp_item& e) { return {e.parent_id, e.name}; }
    static uint64_t hash(const key_type& k) {
        return hash::combine(hash::u64(k.parent_id), hash::string(k.name));
    }
    static bool equal(const key_type& a, const key_type& b) {
        return a.parent_id == b.parent_id && string::strcmp(a.name, b.name) == 0;
    }
};

using comp_map = hashmap::map<comp_item, &comp_item::link, comp_key_ops>;

} // namespace

TEST(hashmap, empty_map_operations) {
    hashmap::bucket buckets[TEST_BUCKETS];
    test_map m;
    m.init(buckets, TEST_BUCKETS);

    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.size(), static_cast<uint32_t>(0));
    EXPECT_EQ(m.bucket_count(), TEST_BUCKETS);
    EXPECT_NULL(m.find(42));

    uint32_t visit_count = 0;
    m.for_each([&](test_item&) { visit_count++; });
    EXPECT_EQ(visit_count, static_cast<uint32_t>(0));
}

TEST(hashmap, insert_single) {
    hashmap::bucket buckets[TEST_BUCKETS];
    test_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item item{42, 100, {}};
    m.insert(&item);

    EXPECT_FALSE(m.empty());
    EXPECT_EQ(m.size(), static_cast<uint32_t>(1));

    auto* found = m.find(42);
    ASSERT_NOT_NULL(found);
    EXPECT_EQ(found->id, static_cast<uint64_t>(42));
    EXPECT_EQ(found->value, static_cast<uint64_t>(100));
}

TEST(hashmap, insert_and_find) {
    hashmap::bucket buckets[TEST_BUCKETS];
    test_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item items[8];
    for (uint64_t i = 0; i < 8; i++) {
        items[i].id = i * 10;
        items[i].value = i;
        items[i].link = {};
        m.insert(&items[i]);
    }

    EXPECT_EQ(m.size(), static_cast<uint32_t>(8));

    for (uint64_t i = 0; i < 8; i++) {
        auto* found = m.find(i * 10);
        ASSERT_NOT_NULL(found);
        EXPECT_EQ(found->value, i);
    }
}

TEST(hashmap, find_missing_key) {
    hashmap::bucket buckets[TEST_BUCKETS];
    test_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item item{10, 1, {}};
    m.insert(&item);

    EXPECT_NULL(m.find(0));
    EXPECT_NULL(m.find(11));
    EXPECT_NULL(m.find(999));
}

TEST(hashmap, remove_single) {
    hashmap::bucket buckets[TEST_BUCKETS];
    test_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item item{42, 1, {}};
    m.insert(&item);
    ASSERT_NOT_NULL(m.find(42));

    m.remove(item);
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.size(), static_cast<uint32_t>(0));
    EXPECT_NULL(m.find(42));
}

TEST(hashmap, remove_head_of_chain) {
    hashmap::bucket buckets[TEST_BUCKETS];
    collide_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item a{1, 10, {}};
    test_item b{2, 20, {}};
    test_item c{3, 30, {}};
    m.insert(&a);
    m.insert(&b);
    m.insert(&c);

    m.remove(c);
    EXPECT_EQ(m.size(), static_cast<uint32_t>(2));
    EXPECT_NULL(m.find(3));
    ASSERT_NOT_NULL(m.find(2));
    ASSERT_NOT_NULL(m.find(1));
}

TEST(hashmap, remove_middle_of_chain) {
    hashmap::bucket buckets[TEST_BUCKETS];
    collide_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item a{1, 10, {}};
    test_item b{2, 20, {}};
    test_item c{3, 30, {}};
    m.insert(&a);
    m.insert(&b);
    m.insert(&c);

    m.remove(b);
    EXPECT_EQ(m.size(), static_cast<uint32_t>(2));
    EXPECT_NULL(m.find(2));
    ASSERT_NOT_NULL(m.find(3));
    ASSERT_NOT_NULL(m.find(1));
}

TEST(hashmap, remove_tail_of_chain) {
    hashmap::bucket buckets[TEST_BUCKETS];
    collide_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item a{1, 10, {}};
    test_item b{2, 20, {}};
    test_item c{3, 30, {}};
    m.insert(&a);
    m.insert(&b);
    m.insert(&c);

    m.remove(a);
    EXPECT_EQ(m.size(), static_cast<uint32_t>(2));
    EXPECT_NULL(m.find(1));
    ASSERT_NOT_NULL(m.find(3));
    ASSERT_NOT_NULL(m.find(2));
}

TEST(hashmap, reinsert_after_remove) {
    hashmap::bucket buckets[TEST_BUCKETS];
    test_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item item{42, 1, {}};
    m.insert(&item);
    m.remove(item);
    EXPECT_NULL(m.find(42));

    item.value = 2;
    m.insert(&item);
    auto* found = m.find(42);
    ASSERT_NOT_NULL(found);
    EXPECT_EQ(found->value, static_cast<uint64_t>(2));
    EXPECT_EQ(m.size(), static_cast<uint32_t>(1));
}

TEST(hashmap, insert_same_entry_twice) {
    hashmap::bucket buckets[TEST_BUCKETS];
    test_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item item{42, 1, {}};
    m.insert(&item);

    EXPECT_TRUE(item.link.pprev != nullptr);
}

TEST(hashmap, insert_ascending) {
    constexpr uint32_t N = 256;
    hashmap::bucket buckets[64];
    test_map m;
    m.init(buckets, 64);

    test_item items[N];
    for (uint32_t i = 0; i < N; i++) {
        items[i].id = i;
        items[i].value = i * 10;
        items[i].link = {};
        m.insert(&items[i]);
    }

    EXPECT_EQ(m.size(), N);
    for (uint32_t i = 0; i < N; i++) {
        auto* found = m.find(i);
        ASSERT_NOT_NULL(found);
        EXPECT_EQ(found->value, static_cast<uint64_t>(i * 10));
    }
}

TEST(hashmap, stress_insert_remove) {
    constexpr uint32_t N = 512;
    hashmap::bucket buckets[64];
    test_map m;
    m.init(buckets, 64);

    test_item items[N];
    for (uint32_t i = 0; i < N; i++) {
        items[i].id = i;
        items[i].value = i;
        items[i].link = {};
        m.insert(&items[i]);
    }
    EXPECT_EQ(m.size(), N);

    for (uint32_t i = 1; i < N; i += 2) {
        m.remove(items[i]);
    }
    EXPECT_EQ(m.size(), N / 2);

    for (uint32_t i = 0; i < N; i += 2) {
        EXPECT_NOT_NULL(m.find(i));
    }
    for (uint32_t i = 1; i < N; i += 2) {
        EXPECT_NULL(m.find(i));
    }

    m.clear();
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.size(), static_cast<uint32_t>(0));

    for (uint32_t i = 0; i < N; i += 2) {
        m.insert(&items[i]);
    }
    EXPECT_EQ(m.size(), N / 2);

    for (uint32_t i = 0; i < N; i += 2) {
        m.remove(items[i]);
    }
    EXPECT_TRUE(m.empty());
}

TEST(hashmap, for_each_visits_all) {
    constexpr uint32_t N = 32;
    hashmap::bucket buckets[TEST_BUCKETS];
    test_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item items[N];
    for (uint32_t i = 0; i < N; i++) {
        items[i].id = i;
        items[i].value = 0;
        items[i].link = {};
        m.insert(&items[i]);
    }

    uint32_t visit_count = 0;
    m.for_each([&](test_item&) { visit_count++; });
    EXPECT_EQ(visit_count, N);
}

TEST(hashmap, for_each_possible_visits_bucket) {
    hashmap::bucket buckets[TEST_BUCKETS];
    collide_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item a{1, 10, {}};
    test_item b{2, 20, {}};
    test_item c{3, 30, {}};
    m.insert(&a);
    m.insert(&b);
    m.insert(&c);

    uint32_t visit_count = 0;
    m.for_each_possible(1, [&](test_item&) { visit_count++; });
    EXPECT_EQ(visit_count, static_cast<uint32_t>(3));
}

TEST(hashmap, for_each_remove_during_iteration) {
    constexpr uint32_t N = 16;
    hashmap::bucket buckets[TEST_BUCKETS];
    test_map m;
    m.init(buckets, TEST_BUCKETS);

    test_item items[N];
    for (uint32_t i = 0; i < N; i++) {
        items[i].id = i;
        items[i].value = 0;
        items[i].link = {};
        m.insert(&items[i]);
    }

    m.for_each([&](test_item& entry) {
        m.remove(entry);
    });
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.size(), static_cast<uint32_t>(0));
}

TEST(hashmap, string_keys) {
    hashmap::bucket buckets[TEST_BUCKETS];
    str_map m;
    m.init(buckets, TEST_BUCKETS);

    str_item hello{"hello", 1, {}};
    str_item world{"world", 2, {}};
    str_item foo{"foo", 3, {}};
    m.insert(&hello);
    m.insert(&world);
    m.insert(&foo);

    EXPECT_EQ(m.size(), static_cast<uint32_t>(3));

    auto* found = m.find("hello");
    ASSERT_NOT_NULL(found);
    EXPECT_EQ(found->value, static_cast<uint64_t>(1));

    found = m.find("world");
    ASSERT_NOT_NULL(found);
    EXPECT_EQ(found->value, static_cast<uint64_t>(2));

    found = m.find("foo");
    ASSERT_NOT_NULL(found);
    EXPECT_EQ(found->value, static_cast<uint64_t>(3));

    EXPECT_NULL(m.find("bar"));
    EXPECT_NULL(m.find(""));
}

TEST(hashmap, composite_key) {
    hashmap::bucket buckets[TEST_BUCKETS];
    comp_map m;
    m.init(buckets, TEST_BUCKETS);

    comp_item a{1, "file.txt", 10, {}};
    comp_item b{1, "readme.md", 20, {}};
    comp_item c{2, "file.txt", 30, {}};
    m.insert(&a);
    m.insert(&b);
    m.insert(&c);

    EXPECT_EQ(m.size(), static_cast<uint32_t>(3));

    auto* found = m.find({1, "file.txt"});
    ASSERT_NOT_NULL(found);
    EXPECT_EQ(found->value, static_cast<uint64_t>(10));

    found = m.find({1, "readme.md"});
    ASSERT_NOT_NULL(found);
    EXPECT_EQ(found->value, static_cast<uint64_t>(20));

    found = m.find({2, "file.txt"});
    ASSERT_NOT_NULL(found);
    EXPECT_EQ(found->value, static_cast<uint64_t>(30));

    EXPECT_NULL(m.find({2, "readme.md"}));
    EXPECT_NULL(m.find({3, "file.txt"}));
    EXPECT_NULL(m.find({1, "other"}));
}
