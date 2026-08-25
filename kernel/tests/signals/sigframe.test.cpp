#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "signals/sigframe.h"

// The frame layout is enforced by static_asserts in signal/sigframe.h.
// These runtime checks confirm the header compiles for the active arch.

TEST_SUITE(sigframe);

#ifdef __x86_64__
TEST(sigframe, x86_frame_matches_musl_abi) {
    EXPECT_EQ(sizeof(x86::rt_sigframe), 440u);
    EXPECT_EQ(sizeof(x86::sigcontext), 256u);
    EXPECT_EQ(__builtin_offsetof(x86::rt_sigframe, info), 312u);
}
#endif

#ifdef __aarch64__
TEST(sigframe, aarch64_frame_matches_musl_abi) {
    EXPECT_EQ(sizeof(aarch64::rt_sigframe), 4688u);
    EXPECT_EQ(sizeof(aarch64::fpsimd_context), 528u);
    EXPECT_EQ(__builtin_offsetof(aarch64::ucontext, uc_mcontext), 176u);
}
#endif
