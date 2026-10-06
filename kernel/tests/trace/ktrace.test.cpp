#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "trace/ktrace.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "common/string.h"

TEST_SUITE(ktrace);

TEST(ktrace, start_fails_while_a_session_is_recording) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    EXPECT_EQ(ktrace::start(), ktrace::ERR_BUSY);
    EXPECT_EQ(ktrace::stop(), ktrace::OK);
}

TEST(ktrace, stop_fails_when_no_session_is_recording) {
    EXPECT_EQ(ktrace::stop(), ktrace::ERR_NOT_RECORDING);
}

TEST(ktrace, a_stopped_session_can_be_started_again) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    EXPECT_EQ(ktrace::start(), ktrace::OK);
    EXPECT_EQ(ktrace::stop(), ktrace::OK);
}

static ssize_t write_to_control(const char* command) {
    fs::file* control = fs::open("/dev/ktrace/control", fs::O_WRONLY);
    if (!control) {
        return fs::ERR_NOENT;
    }

    ssize_t rc = fs::write(control, command, string::strlen(command));
    fs::close(control);
    return rc;
}

TEST(ktrace, writing_start_and_stop_to_control_drives_a_session) {
    ASSERT_EQ(write_to_control("start"), 5);
    EXPECT_EQ(ktrace::start(), ktrace::ERR_BUSY);
    EXPECT_EQ(write_to_control("stop"), 4);
    EXPECT_EQ(ktrace::stop(), ktrace::ERR_NOT_RECORDING);
}

TEST(ktrace, control_accepts_a_command_ending_in_a_newline) {
    ASSERT_EQ(write_to_control("start\n"), 6);
    EXPECT_EQ(write_to_control("stop\n"), 5);
}

TEST(ktrace, control_refuses_start_while_a_session_is_recording) {
    ASSERT_EQ(write_to_control("start"), 5);
    EXPECT_EQ(write_to_control("start"), fs::ERR_BUSY);
    EXPECT_EQ(write_to_control("stop"), 4);
}

TEST(ktrace, control_refuses_stop_when_no_session_is_recording) {
    EXPECT_EQ(write_to_control("stop"), fs::ERR_INVAL);
}

TEST(ktrace, control_refuses_unknown_commands) {
    EXPECT_EQ(write_to_control("restart"), fs::ERR_INVAL);
    EXPECT_EQ(write_to_control("st"), fs::ERR_INVAL);
    EXPECT_EQ(write_to_control("start\n\n"), fs::ERR_INVAL);
}
