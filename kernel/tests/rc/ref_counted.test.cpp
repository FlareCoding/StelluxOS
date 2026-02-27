#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "rc/ref_counted.h"
#include "rc/strong_ref.h"
#include "rc/reaper.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"

TEST_SUITE(ref_counted);

namespace {

struct test_obj : rc::ref_counted<test_obj> {
    uint64_t value;

    test_obj() : value(0) {}

    static void ref_destroy(test_obj* self) {
        RUN_ELEVATED({
            self->~test_obj();
            heap::kfree(self);
        });
    }
};

struct test_uobj : rc::ref_counted<test_uobj> {
    uint64_t value;

    test_uobj() : value(0) {}

    static void ref_destroy(test_uobj* self) {
        self->~test_uobj();
        heap::ufree(self);
    }
};

} // namespace

TEST(ref_counted, initial_refcount_is_one) {
    RUN_ELEVATED({
        auto ref = rc::make_kref<test_obj>();
        ASSERT_TRUE(static_cast<bool>(ref));
        EXPECT_EQ(ref->ref_count(), 1u);
    });
}

TEST(ref_counted, add_ref_increments) {
    RUN_ELEVATED({
        auto ref = rc::make_kref<test_obj>();
        ASSERT_TRUE(static_cast<bool>(ref));

        ref->add_ref();
        EXPECT_EQ(ref->ref_count(), 2u);

        ref->add_ref();
        EXPECT_EQ(ref->ref_count(), 3u);

        // Balance the manual add_refs so the strong_ref destructor is the last release
        [[maybe_unused]] bool r1 = ref->release();
        [[maybe_unused]] bool r2 = ref->release();
    });
}

TEST(ref_counted, release_decrements) {
    RUN_ELEVATED({
        auto ref = rc::make_kref<test_obj>();
        ASSERT_TRUE(static_cast<bool>(ref));

        ref->add_ref();
        EXPECT_EQ(ref->ref_count(), 2u);

        bool last = ref->release();
        EXPECT_FALSE(last);
        EXPECT_EQ(ref->ref_count(), 1u);
    });
}

TEST(ref_counted, last_release_returns_true) {
    RUN_ELEVATED({
        test_obj* raw = heap::kalloc_new<test_obj>();
        ASSERT_NOT_NULL(raw);
        EXPECT_EQ(raw->ref_count(), 1u);

        bool last = raw->release();
        EXPECT_TRUE(last);

        heap::kfree(raw);
    });
}

TEST(ref_counted, try_add_ref_succeeds_when_alive) {
    RUN_ELEVATED({
        auto ref = rc::make_kref<test_obj>();
        ASSERT_TRUE(static_cast<bool>(ref));

        bool ok = ref->try_add_ref();
        EXPECT_TRUE(ok);
        EXPECT_EQ(ref->ref_count(), 2u);

        [[maybe_unused]] bool r = ref->release();
    });
}

TEST(ref_counted, try_add_ref_fails_when_poisoned) {
    RUN_ELEVATED({
        test_obj* raw = heap::kalloc_new<test_obj>();
        ASSERT_NOT_NULL(raw);

        [[maybe_unused]] bool last = raw->release();

        bool ok = raw->try_add_ref();
        EXPECT_FALSE(ok);

        heap::kfree(raw);
    });
}

TEST(ref_counted, strong_ref_copy_increments) {
    RUN_ELEVATED({
        auto a = rc::make_kref<test_obj>();
        ASSERT_TRUE(static_cast<bool>(a));
        EXPECT_EQ(a->ref_count(), 1u);

        auto b = a;
        EXPECT_EQ(a->ref_count(), 2u);
        EXPECT_EQ(a.ptr(), b.ptr());
    });
}

TEST(ref_counted, strong_ref_move_no_increment) {
    RUN_ELEVATED({
        auto a = rc::make_kref<test_obj>();
        ASSERT_TRUE(static_cast<bool>(a));
        test_obj* raw = a.ptr();

        auto b = static_cast<rc::strong_ref<test_obj>&&>(a);
        EXPECT_FALSE(static_cast<bool>(a));
        EXPECT_TRUE(static_cast<bool>(b));
        EXPECT_EQ(b.ptr(), raw);
        EXPECT_EQ(b->ref_count(), 1u);
    });
}

TEST(ref_counted, strong_ref_reset_releases) {
    RUN_ELEVATED({
        auto a = rc::make_kref<test_obj>();
        auto b = a;
        EXPECT_EQ(a->ref_count(), 2u);

        b.reset();
        EXPECT_FALSE(static_cast<bool>(b));
        EXPECT_EQ(a->ref_count(), 1u);
    });
}

TEST(ref_counted, strong_ref_null_is_false) {
    rc::strong_ref<test_obj> ref;
    EXPECT_FALSE(static_cast<bool>(ref));
    EXPECT_NULL(ref.ptr());
}

TEST(ref_counted, strong_ref_adopt) {
    RUN_ELEVATED({
        test_obj* raw = heap::kalloc_new<test_obj>();
        ASSERT_NOT_NULL(raw);
        EXPECT_EQ(raw->ref_count(), 1u);

        auto ref = rc::strong_ref<test_obj>::adopt(raw);
        EXPECT_EQ(ref->ref_count(), 1u);
        EXPECT_EQ(ref.ptr(), raw);
    });
}

TEST(ref_counted, strong_ref_try_from_raw_alive) {
    RUN_ELEVATED({
        auto owner = rc::make_kref<test_obj>();
        test_obj* raw = owner.ptr();

        auto ref = rc::strong_ref<test_obj>::try_from_raw(raw);
        EXPECT_TRUE(static_cast<bool>(ref));
        EXPECT_EQ(ref->ref_count(), 2u);
    });
}

TEST(ref_counted, strong_ref_try_from_raw_dead) {
    RUN_ELEVATED({
        test_obj* raw = heap::kalloc_new<test_obj>();
        ASSERT_NOT_NULL(raw);

        [[maybe_unused]] bool last = raw->release();

        auto ref = rc::strong_ref<test_obj>::try_from_raw(raw);
        EXPECT_FALSE(static_cast<bool>(ref));

        heap::kfree(raw);
    });
}

TEST(ref_counted, strong_ref_swap) {
    RUN_ELEVATED({
        auto a = rc::make_kref<test_obj>();
        auto b = rc::make_kref<test_obj>();
        a->value = 700;
        b->value = 800;
        test_obj* pa = a.ptr();
        test_obj* pb = b.ptr();

        a.swap(b);
        EXPECT_EQ(a.ptr(), pb);
        EXPECT_EQ(b.ptr(), pa);
    });
}

TEST(ref_counted, strong_ref_self_assign) {
    RUN_ELEVATED({
        auto a = rc::make_kref<test_obj>();
        EXPECT_EQ(a->ref_count(), 1u);

        a = a;
        EXPECT_EQ(a->ref_count(), 1u);
    });
}

TEST(ref_counted, strong_ref_multiple_copies) {
    RUN_ELEVATED({
        auto a = rc::make_kref<test_obj>();
        EXPECT_EQ(a->ref_count(), 1u);

        {
            auto b = a;
            auto c = a;
            auto d = b;
            EXPECT_EQ(a->ref_count(), 4u);
        }

        EXPECT_EQ(a->ref_count(), 1u);
    });
}

// --- make_uref tests ---

TEST(ref_counted, make_uref_creates_object) {
    auto ref = rc::make_uref<test_uobj>();
    ASSERT_TRUE(static_cast<bool>(ref));
    EXPECT_EQ(ref->ref_count(), 1u);
}

TEST(ref_counted, make_uref_copy_and_release) {
    auto a = rc::make_uref<test_uobj>();
    ASSERT_TRUE(static_cast<bool>(a));

    auto b = a;
    EXPECT_EQ(a->ref_count(), 2u);

    b.reset();
    EXPECT_EQ(a->ref_count(), 1u);
}

TEST(ref_counted, make_uref_move) {
    auto a = rc::make_uref<test_uobj>();
    ASSERT_TRUE(static_cast<bool>(a));
    test_uobj* raw = a.ptr();

    auto b = static_cast<rc::strong_ref<test_uobj>&&>(a);
    EXPECT_FALSE(static_cast<bool>(a));
    EXPECT_EQ(b.ptr(), raw);
    EXPECT_EQ(b->ref_count(), 1u);
}
