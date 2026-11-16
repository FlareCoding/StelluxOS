#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/serial/fake_chip.h"
#include "drivers/usb/core/usb_identity.h"
#include "drivers/usb/usb_descriptors.h"

TEST_SUITE(serial_number);

using namespace serial_test;

static constexpr uint8_t SERIAL_NUMBER_INDEX = 3;

// US English, the one language the table lists
static constexpr uint8_t LANGUAGE_TABLE[] = {4, usb::USB_DESCRIPTOR_STRING, 0x09, 0x04};

// A string descriptor that holds no text, as a read gone wrong leaves the serial number
static constexpr uint8_t EMPTY_STRING[] = {2, usb::USB_DESCRIPTOR_STRING};

// "AB12", which answers both the read of a string's header and the read of the whole string
static constexpr uint8_t SERIAL_NUMBER_STRING[] = {10, usb::USB_DESCRIPTOR_STRING, 'A', 0, 'B', 0, '1', 0, '2', 0};

static constexpr chip_reply LANGUAGE_REPLY = {.bytes = LANGUAGE_TABLE, .length = sizeof(LANGUAGE_TABLE)};
static constexpr chip_reply EMPTY_REPLY = {.bytes = EMPTY_STRING, .length = sizeof(EMPTY_STRING)};
static constexpr chip_reply SERIAL_NUMBER_REPLY = {
    .bytes = SERIAL_NUMBER_STRING,
    .length = sizeof(SERIAL_NUMBER_STRING),
};

// Static, so a read that never ends still finds them after its test
static fake_chip s_chip;
static usb::usb_device_descriptor s_descriptor;

static void read_strings() {
    usb::core::read_device_strings(&s_chip.controller->usb_device, s_descriptor);
}

// Reads a device that names only a serial number, answering its requests with `replies`
template <size_t COUNT>
static bool read_with_replies(const chip_reply (&replies)[COUNT]) {
    s_descriptor = {};
    s_descriptor.iSerialNumber = SERIAL_NUMBER_INDEX;

    chip_request requests[COUNT] = {};
    return make_calls_with_replies(s_chip, read_strings, requests, replies, COUNT);
}

TEST(serial_number, a_serial_number_read_at_once_is_not_read_again) {
    ASSERT_TRUE(attach_fake_chip(s_chip));

    const chip_reply replies[] = {LANGUAGE_REPLY, SERIAL_NUMBER_REPLY, SERIAL_NUMBER_REPLY};
    EXPECT_TRUE(read_with_replies(replies));
    EXPECT_STREQ(s_chip.controller->usb_device.serial_number, "AB12");
    EXPECT_TRUE(sent_nothing(s_chip));

    detach_fake_chip(s_chip);
}

TEST(serial_number, a_serial_number_the_first_read_misses_is_read_once_more) {
    ASSERT_TRUE(attach_fake_chip(s_chip));

    const chip_reply replies[] = {LANGUAGE_REPLY, EMPTY_REPLY, SERIAL_NUMBER_REPLY, SERIAL_NUMBER_REPLY};
    EXPECT_TRUE(read_with_replies(replies));
    EXPECT_STREQ(s_chip.controller->usb_device.serial_number, "AB12");
    EXPECT_TRUE(sent_nothing(s_chip));

    detach_fake_chip(s_chip);
}

TEST(serial_number, a_serial_number_missed_twice_reads_as_none) {
    ASSERT_TRUE(attach_fake_chip(s_chip));

    const chip_reply replies[] = {LANGUAGE_REPLY, EMPTY_REPLY, EMPTY_REPLY};
    EXPECT_TRUE(read_with_replies(replies));
    EXPECT_STREQ(s_chip.controller->usb_device.serial_number, "");
    EXPECT_TRUE(sent_nothing(s_chip));

    detach_fake_chip(s_chip);
}
