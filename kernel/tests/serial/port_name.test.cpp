#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/serial/port_name.h"
#include "common/string.h"

TEST_SUITE(serial_port_name);

using serial::PORT_NAME_MAX;
using serial::choose_port_name;
using serial::held_name;
using serial::identity;

static constexpr size_t MAX_PORTS = 6;

// What `arrive` returns when the port gets no name. No port name can equal it.
static constexpr const char* NO_NAME = "(none)";

// What a serial number of 0001 adds to its chip's name
static constexpr char SERIAL_SUFFIX[] = "-0001";
static constexpr size_t SERIAL_SUFFIX_LENGTH = sizeof(SERIAL_SUFFIX) - 1;

// What the location 1.2 adds to its chip's name when the serial number has no letters or digits
static constexpr char LOCATION_SUFFIX[] = "-p1.2";
static constexpr size_t LOCATION_SUFFIX_LENGTH = sizeof(LOCATION_SUFFIX) - 1;

// The names held by the ports that arrived before
struct fake_registry {
    held_name              names[MAX_PORTS];
    serial::held_name_list held;
    size_t                 arrived;
};

struct guarded_name {
    held_name name;
    uint8_t   guard[8]; // stays zero unless something writes past the name
};

static void init_registry(fake_registry& registry) {
    registry.held.init();
    registry.arrived = 0;
}

static identity board(const char* chip, const char* serial_number, const char* location, int8_t channel = -1) {
    return {.chip = chip, .serial_number = serial_number, .location = location, .channel = channel};
}

// Names the port `id` describes and adds the name to `registry`
static const char* arrive(fake_registry& registry, const identity& id) {
    if (registry.arrived == MAX_PORTS) {
        return NO_NAME;
    }

    held_name& chosen = registry.names[registry.arrived];
    if (!choose_port_name(id, registry.held, chosen)) {
        return NO_NAME;
    }

    registry.held.push_back(&chosen);
    registry.arrived++;

    return chosen.text;
}

// Frees `name` as its port does when its board leaves
static void leave(fake_registry& registry, const char* name) {
    for (held_name& held : registry.held) {
        if (string::strcmp(held.text, name) == 0) {
            registry.held.remove(&held);
            return;
        }
    }
}

static bool guard_is_intact(const guarded_name& record) {
    for (uint8_t byte : record.guard) {
        if (byte != 0) {
            return false;
        }
    }

    return true;
}

TEST(serial_port_name, a_name_ends_in_the_last_four_letters_or_digits_lowercased) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("cp2102", "0001", "1.5.3")), "cp2102-0001");
    EXPECT_STREQ(arrive(registry, board("cp2102n", "6A1B5D3E7F9", "1.5.4")), "cp2102n-e7f9");
    EXPECT_STREQ(arrive(registry, board("acm", "F4:12:FA:4B:9C:20", "1.6")), "acm-9c20");
    EXPECT_STREQ(arrive(registry, board("ftx", "c1d4e7f9", "1.7")), "ftx-e7f9");
}

TEST(serial_port_name, only_letters_and_digits_count) {
    fake_registry registry;
    init_registry(registry);

    // Each letter and digit range sits between two characters that must not count
    EXPECT_STREQ(arrive(registry, board("x", "/A:z@0[9`a{Z", "2.1")), "x-09az");
}

TEST(serial_port_name, each_collision_adds_two_serial_number_characters) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("cp2102n", "6A1B5D3E7F9", "1.5.3")), "cp2102n-e7f9");
    EXPECT_STREQ(arrive(registry, board("cp2102n", "77C1D4E7F9", "1.5.4")), "cp2102n-d4e7f9");
    EXPECT_STREQ(arrive(registry, board("cp2102n", "99A0D4E7F9", "1.6")), "cp2102n-a0d4e7f9");
}

TEST(serial_port_name, the_whole_serial_number_then_the_location_end_the_growth) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("cp2102n", "5D3E7F9", "1.1")), "cp2102n-e7f9");
    EXPECT_STREQ(arrive(registry, board("cp2102n", "AD3E7F9", "1.2")), "cp2102n-d3e7f9");
    EXPECT_STREQ(arrive(registry, board("cp2102n", "5D3E7F9", "1.3")), "cp2102n-5d3e7f9");
    EXPECT_STREQ(arrive(registry, board("cp2102n", "5D3E7F9", "1.5.4")), "cp2102n-5d3e7f9-p1.5.4");
}

TEST(serial_port_name, a_short_serial_number_is_used_whole) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("cp2102", "0001", "1.5.3")), "cp2102-0001");
    EXPECT_STREQ(arrive(registry, board("cp2102", "0001", "2.4")), "cp2102-0001-p2.4");
    EXPECT_STREQ(arrive(registry, board("cp2102", "7", "2.5")), "cp2102-7");
}

TEST(serial_port_name, a_location_name_another_port_holds_is_refused) {
    fake_registry registry;
    init_registry(registry);

    // A serial number that spells out the second board's location name
    EXPECT_STREQ(arrive(registry, board("x", "P12", "1.1")), "x-p12");
    EXPECT_STREQ(arrive(registry, board("x", "", "12")), NO_NAME);

    // A channel too, though its whole name is free, since two boards never share a base
    EXPECT_STREQ(arrive(registry, board("y", "P12", "1.2", 0)), "y-p12.a");
    EXPECT_STREQ(arrive(registry, board("y", "", "12", 1)), NO_NAME);
}

TEST(serial_port_name, a_serial_number_without_letters_or_digits_gives_the_location) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("ch340", "", "1.3")), "ch340-p1.3");
    EXPECT_STREQ(arrive(registry, board("ch340", ":: -", "1.5.3")), "ch340-p1.5.3");
}

TEST(serial_port_name, a_port_without_a_location_takes_only_serial_number_names) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("cp2102n", "C1D4E7F9", "1.1")), "cp2102n-e7f9");
    EXPECT_STREQ(arrive(registry, board("cp2102n", "D4E7F9", "")), "cp2102n-d4e7f9");
    EXPECT_STREQ(arrive(registry, board("cp2102n", "E7F9", "")), NO_NAME);
    EXPECT_STREQ(arrive(registry, board("ch340", "", "")), NO_NAME);
}

TEST(serial_port_name, ports_without_a_location_are_told_apart_by_chip_and_serial_number) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("cp2102", "0001", "")), "cp2102-0001");
    EXPECT_STREQ(arrive(registry, board("ftx", "0001", "")), "ftx-0001");
    EXPECT_STREQ(arrive(registry, board("cp2102", "0002", "")), "cp2102-0002");
}

TEST(serial_port_name, channels_add_their_letter) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("ft2232h", "FT6XAB12", "1.2", 0)), "ft2232h-ab12.a");
    EXPECT_STREQ(arrive(registry, board("ft2232h", "FT6XAB12", "1.2", 1)), "ft2232h-ab12.b");
    EXPECT_STREQ(arrive(registry, board("ft4232h", "", "1.5.3", 3)), "ft4232h-p1.5.3.d");
}

TEST(serial_port_name, channels_past_z_get_no_name) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("ft4232h", "AB12", "1.1", 25)), "ft4232h-ab12.z");
    EXPECT_STREQ(arrive(registry, board("ft4232h", "AB12", "1.1", 26)), NO_NAME);
}

TEST(serial_port_name, a_devices_channels_share_one_base_when_boards_interleave) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("ft2232h", "FT6XAB12", "1.2", 0)), "ft2232h-ab12.a");
    EXPECT_STREQ(arrive(registry, board("ft2232h", "FT7YAB12", "1.3", 1)), "ft2232h-7yab12.b");
    EXPECT_STREQ(arrive(registry, board("ft2232h", "FT7YAB12", "1.3", 0)), "ft2232h-7yab12.a");
    EXPECT_STREQ(arrive(registry, board("ft2232h", "FT6XAB12", "1.2", 1)), "ft2232h-ab12.b");
}

TEST(serial_port_name, a_channel_keeps_its_siblings_base_after_the_colliding_board_left) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("ft2232h", "FT5ZAB12", "1.4", 0)), "ft2232h-ab12.a");
    EXPECT_STREQ(arrive(registry, board("ft2232h", "FT5ZAB12", "1.4", 1)), "ft2232h-ab12.b");
    EXPECT_STREQ(arrive(registry, board("ft2232h", "FT6XAB12", "1.2", 0)), "ft2232h-6xab12.a");

    leave(registry, "ft2232h-ab12.a");
    leave(registry, "ft2232h-ab12.b");
    EXPECT_STREQ(arrive(registry, board("ft2232h", "FT6XAB12", "1.2", 1)), "ft2232h-6xab12.b");
}

TEST(serial_port_name, two_boards_with_one_serial_number_keep_their_channels_apart) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("cp2105", "0001", "1.2", 0)), "cp2105-0001.a");
    EXPECT_STREQ(arrive(registry, board("cp2105", "0001", "1.3", 1)), "cp2105-0001-p1.3.b");
    EXPECT_STREQ(arrive(registry, board("cp2105", "0001", "1.3", 0)), "cp2105-0001-p1.3.a");
    EXPECT_STREQ(arrive(registry, board("cp2105", "0001", "1.2", 1)), "cp2105-0001.b");
}

TEST(serial_port_name, a_device_numbering_two_ports_alike_gets_no_second_name) {
    fake_registry registry;
    init_registry(registry);

    EXPECT_STREQ(arrive(registry, board("acm", "F4:12:FA:4B:9C:20", "1.6")), "acm-9c20");
    EXPECT_STREQ(arrive(registry, board("acm", "F4:12:FA:4B:9C:20", "1.6")), NO_NAME);
}

TEST(serial_port_name, a_name_fits_up_to_port_name_max_characters) {
    serial::held_name_list none;
    none.init();

    // A chip name that leaves room for exactly the serial suffix
    char chip[PORT_NAME_MAX + 1] = {};
    string::memset(chip, 'c', PORT_NAME_MAX - SERIAL_SUFFIX_LENGTH);

    guarded_name record = {};
    ASSERT_TRUE(choose_port_name(board(chip, "0001", "1.1"), none, record.name));
    EXPECT_EQ(string::strlen(record.name.text), PORT_NAME_MAX);

    chip[PORT_NAME_MAX - SERIAL_SUFFIX_LENGTH] = 'c';
    EXPECT_FALSE(choose_port_name(board(chip, "0001", "1.1"), none, record.name));
    EXPECT_TRUE(guard_is_intact(record));

    string::memset(chip, 'c', PORT_NAME_MAX);
    EXPECT_FALSE(choose_port_name(board(chip, "0001", "1.1"), none, record.name));
    EXPECT_TRUE(guard_is_intact(record));
}

TEST(serial_port_name, a_location_name_fits_up_to_port_name_max_characters) {
    serial::held_name_list none;
    none.init();

    // A chip name that leaves room for exactly the location suffix
    char chip[PORT_NAME_MAX + 1] = {};
    string::memset(chip, 'c', PORT_NAME_MAX - LOCATION_SUFFIX_LENGTH);

    guarded_name record = {};
    ASSERT_TRUE(choose_port_name(board(chip, "", "1.2"), none, record.name));
    EXPECT_EQ(string::strlen(record.name.text), PORT_NAME_MAX);

    chip[PORT_NAME_MAX - LOCATION_SUFFIX_LENGTH] = 'c';
    EXPECT_FALSE(choose_port_name(board(chip, "", "1.2"), none, record.name));
    EXPECT_TRUE(guard_is_intact(record));
}

TEST(serial_port_name, a_serial_number_longer_than_a_name_gives_its_last_characters) {
    serial::held_name_list none;
    none.init();

    static constexpr char TAIL[] = "WXYZ";
    char serial_number[2 * PORT_NAME_MAX] = {};
    string::memset(serial_number, '9', sizeof(serial_number) - 1);
    string::memcpy(&serial_number[sizeof(serial_number) - sizeof(TAIL)], TAIL, sizeof(TAIL) - 1);

    guarded_name record = {};
    ASSERT_TRUE(choose_port_name(board("x", serial_number, "1.1"), none, record.name));
    EXPECT_STREQ(record.name.text, "x-wxyz");
}
