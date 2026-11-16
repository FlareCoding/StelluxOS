#include "stlx_unit_test.h"
#include "drivers/usb/core/usb_driver.h"

TEST_SUITE(usb_class_match);

using usb::USB_MATCH_ANY;
using usb::class_driver_entry;
using usb::core::next_matching_entry;

static constexpr uint16_t DEVICE_VENDOR  = 0x1234;
static constexpr uint16_t DEVICE_PRODUCT = 0x5678;
static constexpr uint8_t  DEVICE_CLASS   = usb::USB_CLASS_VENDOR;

// IDs on either side of the device's that share its low byte, so only an exact 16-bit comparison refuses them
static constexpr uint16_t LOWER_VENDOR   = 0x0234;
static constexpr uint16_t HIGHER_VENDOR  = 0x2234;
static constexpr uint16_t LOWER_PRODUCT  = 0x4678;
static constexpr uint16_t HIGHER_PRODUCT = 0x5778;

static constexpr uint8_t OTHER_CLASS = usb::USB_CLASS_CDC;

// More entries than any table here holds, so an entry offered twice shows up as an extra one
static constexpr size_t MAX_OFFERED = 8;

static usb::device g_device;
static usb::interface g_interface;

static int32_t describe_device() {
    g_device.vid = DEVICE_VENDOR;
    g_device.pid = DEVICE_PRODUCT;
    g_interface.interface_class = DEVICE_CLASS;
    g_interface.interface_subclass = 0;
    g_interface.interface_protocol = 0;
    return 0;
}

BEFORE_ALL(usb_class_match, describe_device);

// Walks `entries` as binding does when every driver declines, and returns how many entries it offered
static size_t collect_offered(
    const class_driver_entry* entries,
    size_t count,
    const class_driver_entry** offered
) {
    const class_driver_entry* last = entries + count;
    size_t offered_count = 0;

    const class_driver_entry* entry = next_matching_entry(entries, last, nullptr, g_device, g_interface);
    while (entry && offered_count < MAX_OFFERED) {
        offered[offered_count++] = entry;
        entry = next_matching_entry(entries, last, entry, g_device, g_interface);
    }

    return offered_count;
}

// A class entry, then a device entry, in the reverse of the order binding offers them
static const class_driver_entry REVERSED_ENTRIES[] = {
    {USB_MATCH(DEVICE_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "class"},
    {USB_MATCH_DEVICE(DEVICE_VENDOR, DEVICE_PRODUCT, DEVICE_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "device"},
};

TEST(usb_class_match, device_entries_are_offered_before_class_entries) {
    const class_driver_entry* offered[MAX_OFFERED] = {};
    size_t offered_count = collect_offered(REVERSED_ENTRIES, 2, offered);

    ASSERT_EQ(offered_count, 2u);
    EXPECT_EQ(offered[0], &REVERSED_ENTRIES[1]);
    EXPECT_EQ(offered[1], &REVERSED_ENTRIES[0]);
}

// Every entry but the last differs from the device's IDs or its interface's class in one field
static const class_driver_entry MISMATCHED_ENTRIES[] = {
    {USB_MATCH_DEVICE(LOWER_VENDOR, DEVICE_PRODUCT, DEVICE_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "vendor"},
    {USB_MATCH_DEVICE(HIGHER_VENDOR, DEVICE_PRODUCT, DEVICE_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "vendor"},
    {USB_MATCH_DEVICE(DEVICE_VENDOR, LOWER_PRODUCT, DEVICE_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "product"},
    {USB_MATCH_DEVICE(DEVICE_VENDOR, HIGHER_PRODUCT, DEVICE_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "product"},
    {USB_MATCH_DEVICE(DEVICE_VENDOR, DEVICE_PRODUCT, OTHER_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "class"},
    {USB_MATCH(OTHER_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "class"},
    {USB_MATCH(DEVICE_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "matching"},
};

TEST(usb_class_match, entries_naming_other_ids_or_classes_are_skipped) {
    const class_driver_entry* offered[MAX_OFFERED] = {};
    size_t offered_count = collect_offered(MISMATCHED_ENTRIES, 7, offered);

    ASSERT_EQ(offered_count, 1u);
    EXPECT_EQ(offered[0], &MISMATCHED_ENTRIES[6]);
}

// A class entry, then two device entries
static const class_driver_entry TWO_DEVICE_ENTRIES[] = {
    {USB_MATCH(DEVICE_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "class"},
    {USB_MATCH_DEVICE(DEVICE_VENDOR, DEVICE_PRODUCT, DEVICE_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "first"},
    {USB_MATCH_DEVICE(DEVICE_VENDOR, DEVICE_PRODUCT, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "second"},
};

TEST(usb_class_match, device_entries_keep_registration_order) {
    const class_driver_entry* offered[MAX_OFFERED] = {};
    size_t offered_count = collect_offered(TWO_DEVICE_ENTRIES, 3, offered);

    ASSERT_EQ(offered_count, 3u);
    EXPECT_EQ(offered[0], &TWO_DEVICE_ENTRIES[1]);
    EXPECT_EQ(offered[1], &TWO_DEVICE_ENTRIES[2]);
    EXPECT_EQ(offered[2], &TWO_DEVICE_ENTRIES[0]);
}

static const class_driver_entry TWO_CLASS_ENTRIES[] = {
    {USB_MATCH(DEVICE_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "first"},
    {USB_MATCH(USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "second"},
};

TEST(usb_class_match, class_entries_keep_registration_order) {
    const class_driver_entry* offered[MAX_OFFERED] = {};
    size_t offered_count = collect_offered(TWO_CLASS_ENTRIES, 2, offered);

    ASSERT_EQ(offered_count, 2u);
    EXPECT_EQ(offered[0], &TWO_CLASS_ENTRIES[0]);
    EXPECT_EQ(offered[1], &TWO_CLASS_ENTRIES[1]);
}

// Names the vendor-specific class, code 0xFF, which must not act as a wildcard
static const class_driver_entry VENDOR_CLASS_ENTRY[] = {
    {USB_MATCH_DEVICE(DEVICE_VENDOR, DEVICE_PRODUCT, DEVICE_CLASS, USB_MATCH_ANY, USB_MATCH_ANY), nullptr, "bridge"},
};

TEST(usb_class_match, an_entry_naming_the_vendor_specific_class_skips_other_classes) {
    const class_driver_entry* last = VENDOR_CLASS_ENTRY + 1;
    usb::interface hid_interface = {};
    hid_interface.interface_class = usb::USB_CLASS_HID;

    EXPECT_EQ(next_matching_entry(VENDOR_CLASS_ENTRY, last, nullptr, g_device, g_interface), &VENDOR_CLASS_ENTRY[0]);
    EXPECT_EQ(next_matching_entry(VENDOR_CLASS_ENTRY, last, nullptr, g_device, hid_interface), nullptr);
}
