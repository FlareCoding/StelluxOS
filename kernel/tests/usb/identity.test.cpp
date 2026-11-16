#include "stlx_unit_test.h"
#include "drivers/usb/core/usb_identity.h"
#include "drivers/usb/usb_langids.h"

TEST_SUITE(usb_identity);

using usb::core::decode_first_language;
using usb::core::decode_string;

static constexpr size_t HEADER_BYTES = 2;

// Room for the longest string descriptor
static constexpr size_t DESCRIPTOR_ROOM = 256;

// Room for every text but the longest
static constexpr size_t TEXT_ROOM = 32;

// A byte no decoder writes, which shows whether a write went past its limit
static constexpr uint8_t GUARD_BYTE = 0xA5;

// The string descriptor type with its high bit set, so only an exact comparison refuses it
static constexpr uint8_t STRING_TYPE_WITH_HIGH_BIT = usb::USB_DESCRIPTOR_STRING | 0x80;

// Writes a string descriptor holding all of `units` to `descriptor`, and returns its length, which `bLength` holds
template <size_t COUNT>
static size_t write_descriptor(const uint16_t (&units)[COUNT], uint8_t* descriptor) {
    size_t length = HEADER_BYTES + COUNT * sizeof(uint16_t);
    descriptor[0] = static_cast<uint8_t>(length);
    descriptor[1] = usb::USB_DESCRIPTOR_STRING;

    for (size_t i = 0; i < COUNT; i++) {
        descriptor[HEADER_BYTES + i * sizeof(uint16_t)] = static_cast<uint8_t>(units[i]);
        descriptor[HEADER_BYTES + i * sizeof(uint16_t) + 1] = static_cast<uint8_t>(units[i] >> 8);
    }

    return length;
}

static const uint16_t BRIDGE_TEXT[] = {'U', 'A', 'R', 'T', ' ', 'B', 'r', 'i', 'd', 'g', 'e'};

TEST(usb_identity, printable_text_is_copied_whole) {
    uint8_t descriptor[DESCRIPTOR_ROOM] = {};
    size_t length = write_descriptor(BRIDGE_TEXT, descriptor);
    char text[TEXT_ROOM];

    decode_string(descriptor, length, text, sizeof(text));

    EXPECT_STREQ(text, "UART Bridge");
}

// The printable range's ends, the values just outside it, a Latin-1 letter and 0x0120, whose low byte is a space
static const uint16_t EDGE_TEXT[] = {0x0020, 0x007E, 0x001F, 0x007F, 0x00E9, 0x0120};

TEST(usb_identity, characters_outside_printable_ascii_read_as_question_marks) {
    uint8_t descriptor[DESCRIPTOR_ROOM] = {};
    size_t length = write_descriptor(EDGE_TEXT, descriptor);
    char text[TEXT_ROOM];

    decode_string(descriptor, length, text, sizeof(text));

    EXPECT_STREQ(text, " ~????");
}

// 0x0100 is NUL in its low byte only, so it reads as a character and the text goes on
static const uint16_t NUL_TEXT[] = {'a', 0x0100, 'b', 0, 'c'};

TEST(usb_identity, a_nul_character_ends_the_text) {
    uint8_t descriptor[DESCRIPTOR_ROOM] = {};
    size_t length = write_descriptor(NUL_TEXT, descriptor);
    char text[TEXT_ROOM];

    decode_string(descriptor, length, text, sizeof(text));

    EXPECT_STREQ(text, "a?b");
}

TEST(usb_identity, the_text_ends_at_blength_or_length) {
    uint8_t descriptor[DESCRIPTOR_ROOM] = {};
    size_t length = write_descriptor(BRIDGE_TEXT, descriptor);
    char text[TEXT_ROOM];

    descriptor[0] = HEADER_BYTES + 4 * sizeof(uint16_t);
    decode_string(descriptor, length, text, sizeof(text));
    EXPECT_STREQ(text, "UART");

    // An odd `bLength` leaves out its last byte, half of a character
    descriptor[0] = HEADER_BYTES + 4 * sizeof(uint16_t) + 1;
    decode_string(descriptor, length, text, sizeof(text));
    EXPECT_STREQ(text, "UART");

    descriptor[0] = static_cast<uint8_t>(length);
    decode_string(descriptor, HEADER_BYTES + 3 * sizeof(uint16_t), text, sizeof(text));
    EXPECT_STREQ(text, "UAR");
}

TEST(usb_identity, anything_but_a_string_descriptor_reads_as_empty) {
    uint8_t descriptor[DESCRIPTOR_ROOM] = {};
    size_t length = write_descriptor(BRIDGE_TEXT, descriptor);
    char text[TEXT_ROOM] = "stale";

    descriptor[1] = usb::USB_DESCRIPTOR_CONFIGURATION;
    decode_string(descriptor, length, text, sizeof(text));
    EXPECT_STREQ(text, "");

    descriptor[1] = STRING_TYPE_WITH_HIGH_BIT;
    decode_string(descriptor, length, text, sizeof(text));
    EXPECT_STREQ(text, "");

    descriptor[1] = usb::USB_DESCRIPTOR_STRING;
    descriptor[0] = 1;
    decode_string(descriptor, length, text, sizeof(text));
    EXPECT_STREQ(text, "");

    descriptor[0] = static_cast<uint8_t>(length);
    decode_string(descriptor, 1, text, sizeof(text));
    EXPECT_STREQ(text, "");
}

TEST(usb_identity, the_text_stops_one_short_of_the_capacity) {
    uint8_t descriptor[DESCRIPTOR_ROOM] = {};
    size_t length = write_descriptor(BRIDGE_TEXT, descriptor);
    char text[TEXT_ROOM];
    string::memset(text, GUARD_BYTE, sizeof(text));

    decode_string(descriptor, length, text, 5);
    EXPECT_STREQ(text, "UART");
    EXPECT_EQ(static_cast<uint8_t>(text[5]), GUARD_BYTE);

    decode_string(descriptor, length, text, 12);
    EXPECT_STREQ(text, "UART Bridge");
    EXPECT_EQ(static_cast<uint8_t>(text[12]), GUARD_BYTE);
}

TEST(usb_identity, the_longest_string_fits_a_device_string) {
    uint16_t units[usb::MAX_STRING_LENGTH];
    for (size_t i = 0; i < usb::MAX_STRING_LENGTH; i++) {
        units[i] = 'x';
    }

    uint8_t descriptor[DESCRIPTOR_ROOM] = {};
    size_t length = write_descriptor(units, descriptor);
    char text[usb::MAX_STRING_LENGTH + 1];

    decode_string(descriptor, length, text, sizeof(text));

    EXPECT_EQ(text[usb::MAX_STRING_LENGTH - 1], 'x');
    EXPECT_EQ(text[usb::MAX_STRING_LENGTH], '\0');
}

// German, then US English: German differs from the fallback, and its ID 0x0407 needs both of its bytes
static const uint8_t TWO_LANGUAGE_TABLE[] = {6, usb::USB_DESCRIPTOR_STRING, 0x07, 0x04, 0x09, 0x04};

TEST(usb_identity, the_first_listed_language_is_chosen) {
    uint8_t table[sizeof(TWO_LANGUAGE_TABLE)] = {};
    string::memcpy(table, TWO_LANGUAGE_TABLE, sizeof(table));
    EXPECT_EQ(decode_first_language(table, sizeof(table)), usb::USB_LANGID_GERMAN);
    EXPECT_EQ(decode_first_language(table, HEADER_BYTES + sizeof(uint16_t)), usb::USB_LANGID_GERMAN);

    table[0] = HEADER_BYTES + sizeof(uint16_t);
    EXPECT_EQ(decode_first_language(table, HEADER_BYTES + sizeof(uint16_t)), usb::USB_LANGID_GERMAN);
}

TEST(usb_identity, a_table_listing_no_language_chooses_us_english) {
    uint8_t table[sizeof(TWO_LANGUAGE_TABLE)] = {};
    string::memcpy(table, TWO_LANGUAGE_TABLE, sizeof(table));

    table[0] = HEADER_BYTES + 1;
    EXPECT_EQ(decode_first_language(table, sizeof(table)), usb::USB_LANGID_ENGLISH_US);

    table[0] = sizeof(TWO_LANGUAGE_TABLE);
    table[1] = usb::USB_DESCRIPTOR_DEVICE;
    EXPECT_EQ(decode_first_language(table, sizeof(table)), usb::USB_LANGID_ENGLISH_US);

    table[1] = STRING_TYPE_WITH_HIGH_BIT;
    EXPECT_EQ(decode_first_language(table, sizeof(table)), usb::USB_LANGID_ENGLISH_US);

    table[1] = usb::USB_DESCRIPTOR_STRING;
    EXPECT_EQ(decode_first_language(table, HEADER_BYTES + 1), usb::USB_LANGID_ENGLISH_US);
}

static usb::device g_device;

TEST(usb_identity, a_location_is_the_controller_then_each_port) {
    // Port 3 of the hub on root port 5 of the second controller
    g_device.controller_number = 2;
    g_device.port_path[0] = 5;
    g_device.port_path[1] = 3;
    g_device.port_path_length = 2;

    char location[usb::core::LOCATION_TEXT_BYTES];
    usb::core::format_location(g_device, location);
    EXPECT_STREQ(location, "2.5.3");

    g_device.port_path_length = 0;
    usb::core::format_location(g_device, location);
    EXPECT_STREQ(location, "2");
}

// The largest controller or port number, three digits as text
static constexpr uint8_t LARGEST_NUMBER = 255;

TEST(usb_identity, the_longest_location_fits_its_text) {
    g_device.controller_number = LARGEST_NUMBER;
    for (uint8_t i = 0; i < usb::MAX_PORT_PATH_LENGTH; i++) {
        g_device.port_path[i] = LARGEST_NUMBER;
    }

    g_device.port_path_length = usb::MAX_PORT_PATH_LENGTH;

    char location[usb::core::LOCATION_TEXT_BYTES];
    usb::core::format_location(g_device, location);
    EXPECT_STREQ(location, "255.255.255.255.255.255.255");
}
