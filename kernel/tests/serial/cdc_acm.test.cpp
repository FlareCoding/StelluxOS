#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/serial/fake_chip.h"
#include "drivers/usb/serial/cdc_acm.h"
#include "drivers/usb/usb_descriptors.h"
#include "terminal/tty.h"

TEST_SUITE(cdc_acm);

using namespace serial_test;
using drivers::xhci_hcd_test_access;
using drivers::xhci::xhci_endpoint;
using drivers::xhci::xhci_normal_trb_t;
using serial::line_settings;
using serial::parity;
using usb::serial::cdc_acm;

struct parity_code {
    parity  mode;
    uint8_t code;
};

struct control_line_state {
    uint32_t lines;
    uint16_t value;
};

static constexpr uint8_t STANDARD_FROM_DEVICE = 0x80;
static constexpr uint8_t CLASS_TO_INTERFACE   = 0x21;

static constexpr uint8_t GET_DESCRIPTOR         = 0x06;
static constexpr uint8_t SET_LINE_CODING        = 0x20;
static constexpr uint8_t SET_CONTROL_LINE_STATE = 0x22;
static constexpr uint8_t SEND_BREAK             = 0x23;

// The first configuration descriptor, read as far as the xHCI driver reads one
static constexpr uint16_t CONFIGURATION_DESCRIPTOR  = 0x0200;
static constexpr uint16_t CONFIGURATION_READ_LENGTH = 254;

static constexpr uint16_t LINE_CODING_LENGTH = 7;

static constexpr uint8_t CS_INTERFACE    = 0x24;
static constexpr uint8_t ACM_SUBCLASS    = 0x02;
static constexpr uint8_t VENDOR_PROTOCOL = 0xFF;
static constexpr uint8_t DATA_CLASS      = 0x0A;

// The second port's interfaces, so a request naming interface 0 fails the check
static constexpr uint8_t CONTROL_INTERFACE = 2;
static constexpr uint8_t DATA_INTERFACE    = 3;

// The data interface's bulk pair
static constexpr uint8_t  DATA_IN          = fake_xhci::BULK_IN;
static constexpr uint8_t  DATA_OUT         = 0x02;
static constexpr uint16_t DATA_PACKET_SIZE = fake_xhci::MAX_PACKET;

// A SERIAL_STATE takes two of the notification endpoint's 8-byte packets
static constexpr uint8_t  NOTIFICATION_IN         = 0x83;
static constexpr uint8_t  INTERRUPT_ATTRIBUTES    = 3;
static constexpr uint16_t NOTIFICATION_MAX_PACKET = 8;
static constexpr uint8_t  NOTIFICATION_INTERVAL   = 16;
static constexpr uint32_t NOTIFICATION_READS      = 2;
static constexpr uint32_t SERIAL_STATE_LENGTH     = 10;
static constexpr uint8_t  RESPONSE_AVAILABLE      = 0x01;

// SERIAL_STATE's line states: CD, DSR, break, RI, then framing, parity and overrun errors
static constexpr uint8_t STATE_CD             = 0x01;
static constexpr uint8_t STATE_DSR            = 0x02;
static constexpr uint8_t STATE_RI             = 0x08;
static constexpr uint8_t STATE_ALL_BUT_INPUTS = 0xF4;

// Two ports: control interfaces 0 and 2, whose union descriptors name data interfaces 1 and 3
static const uint8_t TWO_PORT_CONFIG[] = {
    9, usb::USB_DESCRIPTOR_CONFIGURATION, 97, 0, 4, 1, 0, 0x80, 50,                    // configuration, 97 bytes
    9, usb::USB_DESCRIPTOR_INTERFACE, 0, 0, 1, usb::USB_CLASS_CDC, ACM_SUBCLASS, 1, 0, // control interface 0
    5, CS_INTERFACE, 0x06, 0, 1,                                                        // union of 0 and 1
    7, usb::USB_DESCRIPTOR_ENDPOINT, 0x85, 3, 8, 0, 16,
    9, usb::USB_DESCRIPTOR_INTERFACE, 1, 0, 2, DATA_CLASS, 0, 0, 0,                     // data interface 1
    7, usb::USB_DESCRIPTOR_ENDPOINT, 0x06, 2, 64, 0, 0,
    7, usb::USB_DESCRIPTOR_ENDPOINT, 0x86, 2, 64, 0, 0,
    9, usb::USB_DESCRIPTOR_INTERFACE, 2, 0, 1, usb::USB_CLASS_CDC, ACM_SUBCLASS, 1, 0, // control interface 2
    5, CS_INTERFACE, 0x06, 2, 3,                                                        // union of 2 and 3
    7, usb::USB_DESCRIPTOR_ENDPOINT, NOTIFICATION_IN, 3, 8, 0, 16,
    9, usb::USB_DESCRIPTOR_INTERFACE, 3, 0, 2, DATA_CLASS, 0, 0, 0,                     // data interface 3
    7, usb::USB_DESCRIPTOR_ENDPOINT, DATA_OUT, 2, 64, 0, 0,
    7, usb::USB_DESCRIPTOR_ENDPOINT, DATA_IN, 2, 64, 0, 0,
};

// Where control interface 2's descriptor and its union descriptor start in `TWO_PORT_CONFIG`, and the union's length
static constexpr size_t SECOND_CONTROL_INTERFACE = 53;
static constexpr size_t SECOND_UNION             = 62;
static constexpr size_t UNION_LENGTH             = 5;

// Static, so calls that never end still find them after their test, which then leaves the translator unfreed
static fake_chip      s_chip;
static cdc_acm*       s_translator;
static xhci_endpoint* s_notification_endpoint;

// What the calls take and give back
static line_settings s_settings;
static uint32_t      s_lines;
static bool          s_is_break_on;
static int32_t       s_rc;

// Lists control interface 2 and data interface 3 as the xHCI driver lists the interfaces it configured, the control
// interface with its notification endpoint when `has_notifications` is set
static void list_port_interfaces(bool has_notifications) {
    usb::device& dev = s_chip.controller->usb_device;
    usb::interface& control = dev.interfaces[0];
    usb::interface& data = dev.interfaces[1];

    control.interface_number = CONTROL_INTERFACE;
    control.interface_class = usb::USB_CLASS_CDC;
    control.interface_subclass = ACM_SUBCLASS;
    control.num_endpoints = has_notifications ? 1 : 0;
    control.endpoints[0] = {
        NOTIFICATION_IN,
        usb::USB_ENDPOINT_TYPE_INTERRUPT,
        NOTIFICATION_MAX_PACKET,
        NOTIFICATION_INTERVAL
    };

    data.interface_number = DATA_INTERFACE;
    data.interface_class = DATA_CLASS;
    data.num_endpoints = 2;
    data.endpoints[0] = {DATA_OUT, usb::USB_ENDPOINT_TYPE_BULK, DATA_PACKET_SIZE, 0};
    data.endpoints[1] = {DATA_IN, usb::USB_ENDPOINT_TYPE_BULK, DATA_PACKET_SIZE, 0};

    dev.num_interfaces = 2;
}

static bool attach_translator(bool has_notifications = false) {
    if (!attach_fake_chip(s_chip)) {
        return false;
    }

    list_port_interfaces(has_notifications);
    usb::device& dev = s_chip.controller->usb_device;
    s_translator = heap::ualloc_new<cdc_acm>(&dev, dev.interfaces[0]);
    if (!s_translator) {
        detach_fake_chip(s_chip);
        return false;
    }

    return true;
}

static void destroy_translator() {
    s_translator->close_streams();
    heap::ufree_delete(s_translator);
}

// Destroys the translator on a task of its own, since closing its notification stream can wait, then frees the chip
static void detach_translator() {
    if (!s_chip.is_call_stuck) {
        make_calls(s_chip, destroy_translator, nullptr, 0);
    }

    detach_fake_chip(s_chip);
}

static void configure() {
    s_rc = s_translator->configure();
}

static void set_line() {
    s_rc = s_translator->set_line(s_settings);
}

static void set_modem_outputs() {
    s_rc = s_translator->set_modem_outputs(s_lines);
}

static void set_break() {
    s_rc = s_translator->set_break(s_is_break_on);
}

// Configures the translator with `config` as the device's configuration descriptor and returns its result, or
// ERR_IO when the calls fail
static int32_t configure_with(const uint8_t* config, uint16_t length) {
    chip_request request;
    if (!make_calls(s_chip, configure, &request, 1, config, length)) {
        return serial::ERR_IO;
    }

    return s_rc;
}

static void expect_request(const chip_request& request, uint8_t code, uint16_t value, uint16_t length) {
    EXPECT_EQ(request.request_type, CLASS_TO_INTERFACE);
    EXPECT_EQ(request.request, code);
    EXPECT_EQ(request.value, value);
    EXPECT_EQ(request.index, static_cast<uint16_t>(CONTROL_INTERFACE));
    EXPECT_EQ(request.length, length);
}

// Records in `request` the SET_LINE_CODING request a configured translator sends for `settings`
static void record_line_coding(const line_settings& settings, chip_request* request) {
    s_settings = settings;
    EXPECT_TRUE(make_calls(s_chip, set_line, request, 1));
    EXPECT_EQ(s_rc, serial::OK);
    expect_request(*request, SET_LINE_CODING, 0, LINE_CODING_LENGTH);
}

TEST(cdc_acm, configure_reads_the_data_interface_from_the_union_descriptor) {
    ASSERT_TRUE(attach_translator());

    chip_request request;
    bool is_done = make_calls(s_chip, configure, &request, 1, TWO_PORT_CONFIG, sizeof(TWO_PORT_CONFIG));
    uint8_t data_interface = s_translator->data_interface_number();
    uint32_t inputs = 0;
    s_translator->get_modem_inputs(&inputs);

    detach_translator();
    ASSERT_TRUE(is_done);
    EXPECT_EQ(s_rc, serial::OK);
    EXPECT_EQ(data_interface, DATA_INTERFACE);

    EXPECT_EQ(request.request_type, STANDARD_FROM_DEVICE);
    EXPECT_EQ(request.request, GET_DESCRIPTOR);
    EXPECT_EQ(request.value, CONFIGURATION_DESCRIPTOR);
    EXPECT_EQ(request.index, 0u);
    EXPECT_EQ(request.length, CONFIGURATION_READ_LENGTH);

    // The control interface lists no notification endpoint, so only the CTS the class lacks is raised
    EXPECT_EQ(inputs, terminal::TIOCM_CTS);
}

// Control interface 2 in alternate setting 1 only, a descriptor of length 0, which would keep the walk in place, and a
// configuration whose length ends inside the union descriptor
TEST(cdc_acm, a_control_interface_without_its_union_descriptor_is_refused) {
    ASSERT_TRUE(attach_translator());

    uint8_t alternate[sizeof(TWO_PORT_CONFIG)];
    string::memcpy(alternate, TWO_PORT_CONFIG, sizeof(alternate));
    alternate[SECOND_CONTROL_INTERFACE + 3] = 1;

    uint8_t zero_length[sizeof(TWO_PORT_CONFIG)];
    string::memcpy(zero_length, TWO_PORT_CONFIG, sizeof(zero_length));
    zero_length[SECOND_CONTROL_INTERFACE] = 0;

    uint8_t cut_short[sizeof(TWO_PORT_CONFIG)];
    string::memcpy(cut_short, TWO_PORT_CONFIG, sizeof(cut_short));
    cut_short[2] = SECOND_UNION + UNION_LENGTH - 1;

    int32_t alternate_rc = configure_with(alternate, sizeof(alternate));
    int32_t zero_length_rc = configure_with(zero_length, sizeof(zero_length));
    int32_t cut_short_rc = configure_with(cut_short, sizeof(cut_short));

    detach_translator();
    EXPECT_EQ(alternate_rc, serial::ERR_UNSUPPORTED);
    EXPECT_EQ(zero_length_rc, serial::ERR_UNSUPPORTED);
    EXPECT_EQ(cut_short_rc, serial::ERR_UNSUPPORTED);
}

// A length of 316 bytes, whose low byte alone would end the walk before control interface 2's union descriptor, so
// the walk ends where the read did
TEST(cdc_acm, a_configuration_longer_than_the_read_is_walked_to_the_reads_end) {
    ASSERT_TRUE(attach_translator());

    uint8_t long_config[sizeof(TWO_PORT_CONFIG)];
    string::memcpy(long_config, TWO_PORT_CONFIG, sizeof(long_config));
    long_config[2] = SECOND_UNION - 2;
    long_config[3] = 1;

    int32_t rc = configure_with(long_config, sizeof(long_config));
    uint8_t data_interface = s_translator->data_interface_number();

    detach_translator();
    EXPECT_EQ(rc, serial::OK);
    EXPECT_EQ(data_interface, DATA_INTERFACE);
}

// A vendor protocol on the control interface, as RNDIS network adapters report, carries no serial port
TEST(cdc_acm, a_control_interface_with_a_vendor_protocol_is_refused_without_a_request) {
    ASSERT_TRUE(attach_translator());
    s_chip.controller->usb_device.interfaces[0].interface_protocol = VENDOR_PROTOCOL;

    bool is_done = make_calls(s_chip, configure, nullptr, 0);
    bool is_silent = sent_nothing(s_chip);

    detach_translator();
    ASSERT_TRUE(is_done);
    EXPECT_EQ(s_rc, serial::ERR_UNSUPPORTED);
    EXPECT_TRUE(is_silent);
}

// Once teardown has ended EP0's transfers, every request fails at once
TEST(cdc_acm, configure_fails_with_an_io_error_when_the_descriptor_cannot_be_read) {
    ASSERT_TRUE(attach_translator());
    xhci_hcd_test_access::end_control_transfers(*s_chip.controller->hcd, s_chip.controller->device);

    bool is_done = make_calls(s_chip, configure, nullptr, 0);

    detach_translator();
    ASSERT_TRUE(is_done);
    EXPECT_EQ(s_rc, serial::ERR_IO);
}

TEST(cdc_acm, requests_after_an_unplug_fail_with_an_io_error) {
    ASSERT_TRUE(attach_translator());
    ASSERT_EQ(configure_with(TWO_PORT_CONFIG, sizeof(TWO_PORT_CONFIG)), serial::OK);
    xhci_hcd_test_access::end_control_transfers(*s_chip.controller->hcd, s_chip.controller->device);

    s_settings = {115200, 8, parity::none, 1, false};
    bool are_done = make_calls(s_chip, set_line, nullptr, 0);
    int32_t line_rc = s_rc;

    s_lines = terminal::TIOCM_DTR | terminal::TIOCM_RTS;
    are_done = are_done && make_calls(s_chip, set_modem_outputs, nullptr, 0);
    int32_t outputs_rc = s_rc;

    s_is_break_on = true;
    are_done = are_done && make_calls(s_chip, set_break, nullptr, 0);
    int32_t break_rc = s_rc;

    detach_translator();
    ASSERT_TRUE(are_done);
    EXPECT_EQ(line_rc, serial::ERR_IO);
    EXPECT_EQ(outputs_rc, serial::ERR_IO);
    EXPECT_EQ(break_rc, serial::ERR_IO);
}

// A device with both ports of `TWO_PORT_CONFIG`: control interfaces 0 and 2, and data interfaces 1 and 3
static usb::device s_two_port_device;

static void list_both_ports(usb::device& dev) {
    for (uint8_t i = 0; i < 4; i++) {
        bool is_control = i % 2 == 0;
        usb::interface& iface = dev.interfaces[i];
        iface.interface_number = i;
        iface.interface_class = is_control ? usb::USB_CLASS_CDC : DATA_CLASS;
        iface.interface_subclass = is_control ? ACM_SUBCLASS : 0;
        iface.interface_protocol = 0;
    }

    dev.num_interfaces = 4;
}

// Each port of a device with two CDC-ACM functions takes a channel, while a lone function, even beside an RNDIS
// control interface, takes none
TEST(cdc_acm, each_function_of_a_device_with_several_takes_a_channel) {
    list_both_ports(s_two_port_device);
    cdc_acm first(&s_two_port_device, s_two_port_device.interfaces[0]);
    cdc_acm second(&s_two_port_device, s_two_port_device.interfaces[2]);
    EXPECT_EQ(first.channel(), 0);
    EXPECT_EQ(second.channel(), 1);

    s_two_port_device.interfaces[2].interface_protocol = VENDOR_PROTOCOL;
    cdc_acm beside_rndis(&s_two_port_device, s_two_port_device.interfaces[0]);
    EXPECT_EQ(beside_rndis.channel(), -1);

    s_two_port_device.num_interfaces = 2;
    cdc_acm lone(&s_two_port_device, s_two_port_device.interfaces[0]);
    EXPECT_EQ(lone.channel(), -1);
}

static const uint8_t PLAIN_CODING[] = {0x00, 0xC2, 0x01, 0x00, 0, 0, 8};

// 20,000,000 baud fills all four of the speed's little-endian bytes, which the class passes through
static const uint8_t FAST_CODING[] = {0x00, 0x2D, 0x31, 0x01, 2, 2, 7};

TEST(cdc_acm, set_line_coding_carries_the_speed_then_the_character_format) {
    ASSERT_TRUE(attach_translator());
    ASSERT_EQ(configure_with(TWO_PORT_CONFIG, sizeof(TWO_PORT_CONFIG)), serial::OK);

    chip_request plain;
    chip_request fast;
    record_line_coding({115200, 8, parity::none, 1, false}, &plain);
    record_line_coding({20000000, 7, parity::even, 2, false}, &fast);

    detach_translator();
    EXPECT_EQ(string::memcmp(plain.data, PLAIN_CODING, sizeof(PLAIN_CODING)), 0);
    EXPECT_EQ(string::memcmp(fast.data, FAST_CODING, sizeof(FAST_CODING)), 0);
}

static constexpr parity_code PARITY_CODES[] = {
    {parity::none,  0},
    {parity::odd,   1},
    {parity::even,  2},
    {parity::mark,  3},
    {parity::space, 4},
};

TEST(cdc_acm, each_parity_has_its_class_code) {
    ASSERT_TRUE(attach_translator());
    ASSERT_EQ(configure_with(TWO_PORT_CONFIG, sizeof(TWO_PORT_CONFIG)), serial::OK);

    for (const parity_code& expected : PARITY_CODES) {
        chip_request request;
        record_line_coding({9600, 5, expected.mode, 1, false}, &request);
        EXPECT_EQ(request.data[5], expected.code);
        EXPECT_EQ(request.data[6], 5u);
    }

    detach_translator();
}

TEST(cdc_acm, flow_control_by_cts_is_refused_without_a_request) {
    ASSERT_TRUE(attach_translator());
    ASSERT_EQ(configure_with(TWO_PORT_CONFIG, sizeof(TWO_PORT_CONFIG)), serial::OK);

    s_settings = {115200, 8, parity::none, 1, true};
    bool is_done = make_calls(s_chip, set_line, nullptr, 0);
    bool is_silent = sent_nothing(s_chip);

    detach_translator();
    ASSERT_TRUE(is_done);
    EXPECT_EQ(s_rc, serial::ERR_UNSUPPORTED);
    EXPECT_TRUE(is_silent);
}

// DTR in bit 0 and RTS in bit 1, whatever input lines come with them
static constexpr control_line_state CONTROL_LINE_STATES[] = {
    {terminal::TIOCM_DTR | terminal::TIOCM_RTS, 3},
    {terminal::TIOCM_DTR,                       1},
    {terminal::TIOCM_RTS | terminal::TIOCM_CTS, 2},
    {terminal::TIOCM_CAR | terminal::TIOCM_RNG, 0},
};

TEST(cdc_acm, set_control_line_state_sets_dtr_and_rts_in_one_request) {
    ASSERT_TRUE(attach_translator());
    ASSERT_EQ(configure_with(TWO_PORT_CONFIG, sizeof(TWO_PORT_CONFIG)), serial::OK);

    bool are_done = true;
    for (const control_line_state& expected : CONTROL_LINE_STATES) {
        chip_request request;
        s_lines = expected.lines;
        are_done = are_done && make_calls(s_chip, set_modem_outputs, &request, 1);

        EXPECT_EQ(s_rc, serial::OK);
        expect_request(request, SET_CONTROL_LINE_STATE, expected.value, 0);
    }

    detach_translator();
    EXPECT_TRUE(are_done);
}

TEST(cdc_acm, a_break_lasts_until_it_is_ended) {
    ASSERT_TRUE(attach_translator());
    ASSERT_EQ(configure_with(TWO_PORT_CONFIG, sizeof(TWO_PORT_CONFIG)), serial::OK);

    chip_request on;
    s_is_break_on = true;
    bool are_done = make_calls(s_chip, set_break, &on, 1);

    chip_request off;
    s_is_break_on = false;
    are_done = are_done && make_calls(s_chip, set_break, &off, 1);

    detach_translator();
    ASSERT_TRUE(are_done);
    expect_request(on, SEND_BREAK, 0xFFFF, 0);
    expect_request(off, SEND_BREAK, 0, 0);
}

TEST(cdc_acm, the_bytes_left_to_send_are_unknown_without_a_request) {
    ASSERT_TRUE(attach_translator());
    ASSERT_EQ(configure_with(TWO_PORT_CONFIG, sizeof(TWO_PORT_CONFIG)), serial::OK);

    ssize_t pending = s_translator->tx_pending();
    bool is_silent = sent_nothing(s_chip);

    detach_translator();
    EXPECT_EQ(pending, serial::TX_UNKNOWN);
    EXPECT_TRUE(is_silent);
}

static const uint8_t SERIAL_STATE_HEADER[] = {0xA1, 0x20, 0, 0, CONTROL_INTERFACE, 0, 2, 0};

// A RESPONSE_AVAILABLE, a header with no data, which fills one packet of the notification endpoint exactly
static const uint8_t RESPONSE_AVAILABLE_NOTIFICATION[] = {0xA1, RESPONSE_AVAILABLE, 0, 0, CONTROL_INTERFACE, 0, 0, 0};

// A SERIAL_STATE whose header gives no data, so it carries no line states
static const uint8_t EMPTY_SERIAL_STATE[] = {0xA1, 0x20, 0, 0, CONTROL_INTERFACE, 0, 0, 0};

// Writes a SERIAL_STATE notification whose first byte of line states is `states` into `notification`
static void write_serial_state(uint8_t* notification, uint8_t states) {
    string::memcpy(notification, SERIAL_STATE_HEADER, sizeof(SERIAL_STATE_HEADER));
    notification[8] = states;
    notification[9] = 0;
}

// The ring entry of the notification read the device's next packet goes into, and the bytes that read holds so far
static uint32_t s_read_index;
static uint32_t s_read_filled;

// A configured translator on a chip whose control interface lists the notification endpoint, which the fake
// controller sets up, before its streams open
static bool attach_notifying_translator() {
    if (!attach_translator(true)) {
        return false;
    }

    s_read_index = 0;
    s_read_filled = 0;
    s_notification_endpoint = usb_test::add_fake_endpoint(
        *s_chip.controller,
        NOTIFICATION_IN,
        INTERRUPT_ATTRIBUTES,
        NOTIFICATION_MAX_PACKET
    );
    return s_notification_endpoint && configure_with(TWO_PORT_CONFIG, sizeof(TWO_PORT_CONFIG)) == serial::OK;
}

// The stream buffer the notification read at ring entry `index` was posted with, or null when none is posted there
static uint8_t* posted_buffer(uint32_t index) {
    auto* read = reinterpret_cast<const xhci_normal_trb_t*>(
        usb_test::transfer_trb(*s_chip.controller, index, s_notification_endpoint)
    );
    for (uint32_t i = 0; read && i < NOTIFICATION_READS; i++) {
        if (s_notification_endpoint->stream_buffer_phys(i) == read->data_buffer_physical_base) {
            return s_notification_endpoint->stream_buffer(i);
        }
    }

    return nullptr;
}

// Plays the device sending a packet into the posted read at the current ring entry, which completes once full or at a
// short packet, as an interrupt IN pipe's does, and makes its call. False when no posted read can take the packet
static bool send_packet(const uint8_t* packet, uint32_t length) {
    auto* read = reinterpret_cast<const xhci_normal_trb_t*>(
        usb_test::transfer_trb(*s_chip.controller, s_read_index, s_notification_endpoint)
    );
    uint8_t* buffer = posted_buffer(s_read_index);
    if (!read || !buffer || s_read_filled + length > read->trb_transfer_length) {
        return false;
    }

    string::memcpy(buffer + s_read_filled, packet, length);
    s_read_filled += length;
    uint32_t residual = read->trb_transfer_length - s_read_filled;
    if (length == NOTIFICATION_MAX_PACKET && residual > 0) {
        return true;
    }

    uint8_t code = residual == 0 ? XHCI_TRB_COMPLETION_CODE_SUCCESS : XHCI_TRB_COMPLETION_CODE_SHORT_PACKET;
    usb_test::post_transfer_event(*s_chip.controller, s_read_index, residual, code, s_notification_endpoint);
    xhci_hcd_test_access::process_events(*s_chip.controller->hcd);
    s_read_index++;
    s_read_filled = 0;
    return true;
}

// Sends the first `length` bytes of `notification` as the device does, in full packets and then any shorter one
static bool send_notification(const uint8_t* notification, uint32_t length) {
    bool is_sent = true;
    for (uint32_t offset = 0; offset < length && is_sent; offset += NOTIFICATION_MAX_PACKET) {
        uint32_t remaining = length - offset;
        uint32_t packet_length = remaining < NOTIFICATION_MAX_PACKET ? remaining : NOTIFICATION_MAX_PACKET;
        is_sent = send_packet(notification + offset, packet_length);
    }

    return is_sent;
}

static uint32_t modem_inputs() {
    uint32_t inputs = 0;
    s_translator->get_modem_inputs(&inputs);
    return inputs;
}

// The unplug ends the reads, so closing the stream at the translator's end needs no stop
static void unplug_chip() {
    xhci_hcd_test_access::mark_unplugged(*s_chip.controller->hcd, s_chip.controller->device);
    xhci_hcd_test_access::end_unplugged_transfers(*s_chip.controller->hcd, s_chip.controller->device);
}

// configure() runs on the HCD task and posts no read, and the stream opened on the bridge driver's task keeps two
// reads of one packet each posted on the notification endpoint
TEST(cdc_acm, notifications_on_the_control_interface_give_the_modem_inputs) {
    ASSERT_TRUE(attach_notifying_translator());
    bool was_read_posted_by_configure = posted_buffer(0) != nullptr;
    s_translator->open_streams();

    auto* first_read = reinterpret_cast<const xhci_normal_trb_t*>(
        usb_test::transfer_trb(*s_chip.controller, 0, s_notification_endpoint)
    );
    auto* second_read = reinterpret_cast<const xhci_normal_trb_t*>(
        usb_test::transfer_trb(*s_chip.controller, 1, s_notification_endpoint)
    );
    uint32_t first_read_length = first_read ? first_read->trb_transfer_length : 0;
    uint32_t second_read_length = second_read ? second_read->trb_transfer_length : 0;

    uint8_t notification[SERIAL_STATE_LENGTH];
    write_serial_state(notification, STATE_CD | STATE_DSR);
    bool is_sent = send_notification(notification, sizeof(notification));
    uint32_t inputs = modem_inputs();

    unplug_chip();
    detach_translator();

    EXPECT_FALSE(was_read_posted_by_configure);
    EXPECT_EQ(first_read_length, static_cast<uint32_t>(NOTIFICATION_MAX_PACKET));
    EXPECT_EQ(second_read_length, static_cast<uint32_t>(NOTIFICATION_MAX_PACKET));
    EXPECT_TRUE(is_sent);
    EXPECT_EQ(inputs, terminal::TIOCM_CTS | terminal::TIOCM_CAR | terminal::TIOCM_DSR);
}

TEST(cdc_acm, each_serial_state_replaces_the_modem_inputs) {
    ASSERT_TRUE(attach_notifying_translator());
    s_translator->open_streams();
    uint8_t notification[SERIAL_STATE_LENGTH];

    write_serial_state(notification, STATE_RI);
    bool are_sent = send_notification(notification, sizeof(notification));
    uint32_t ring_only = modem_inputs();

    write_serial_state(notification, STATE_ALL_BUT_INPUTS);
    are_sent = are_sent && send_notification(notification, sizeof(notification));
    uint32_t none = modem_inputs();

    unplug_chip();
    detach_translator();
    EXPECT_TRUE(are_sent);
    EXPECT_EQ(ring_only, terminal::TIOCM_CTS | terminal::TIOCM_RNG);
    EXPECT_EQ(none, terminal::TIOCM_CTS);
}

// The RESPONSE_AVAILABLE ends on a full packet with no short packet after it, so only its length keeps the
// SERIAL_STATE that follows from running into it
TEST(cdc_acm, a_notification_that_ends_on_a_full_packet_stays_apart_from_the_next) {
    ASSERT_TRUE(attach_notifying_translator());
    s_translator->open_streams();
    bool are_sent = send_notification(RESPONSE_AVAILABLE_NOTIFICATION, sizeof(RESPONSE_AVAILABLE_NOTIFICATION));

    uint8_t notification[SERIAL_STATE_LENGTH];
    write_serial_state(notification, STATE_CD);
    are_sent = are_sent && send_notification(notification, sizeof(notification));
    uint32_t inputs = modem_inputs();

    unplug_chip();
    detach_translator();
    EXPECT_TRUE(are_sent);
    EXPECT_EQ(inputs, terminal::TIOCM_CTS | terminal::TIOCM_CAR);
}

// The first packet carries only the header, so the inputs change once the second brings the line states
TEST(cdc_acm, a_serial_state_split_across_two_packets_arrives_whole) {
    ASSERT_TRUE(attach_notifying_translator());
    s_translator->open_streams();
    uint8_t notification[SERIAL_STATE_LENGTH];
    write_serial_state(notification, STATE_DSR | STATE_RI);

    uint32_t states_length = SERIAL_STATE_LENGTH - NOTIFICATION_MAX_PACKET;
    bool are_sent = send_packet(notification, NOTIFICATION_MAX_PACKET);
    uint32_t after_header = modem_inputs();
    are_sent = are_sent && send_packet(&notification[NOTIFICATION_MAX_PACKET], states_length);
    uint32_t after_states = modem_inputs();

    unplug_chip();
    detach_translator();
    EXPECT_TRUE(are_sent);
    EXPECT_EQ(after_header, terminal::TIOCM_CTS);
    EXPECT_EQ(after_states, terminal::TIOCM_CTS | terminal::TIOCM_DSR | terminal::TIOCM_RNG);
}

// A SERIAL_STATE cut short by a short packet, a RESPONSE_AVAILABLE with data, a request type the class never sends, and
// a SERIAL_STATE without line states after them, which must not read DSR's bits the others left
TEST(cdc_acm, other_notifications_leave_the_modem_inputs) {
    ASSERT_TRUE(attach_notifying_translator());
    s_translator->open_streams();
    uint8_t notification[SERIAL_STATE_LENGTH];

    write_serial_state(notification, STATE_CD);
    bool are_sent = send_notification(notification, sizeof(notification));

    write_serial_state(notification, STATE_DSR);
    are_sent = are_sent && send_notification(notification, sizeof(notification) - 1);

    notification[1] = RESPONSE_AVAILABLE;
    are_sent = are_sent && send_notification(notification, sizeof(notification));

    write_serial_state(notification, STATE_DSR);
    notification[0] = CLASS_TO_INTERFACE;
    are_sent = are_sent && send_notification(notification, sizeof(notification));

    are_sent = are_sent && send_notification(EMPTY_SERIAL_STATE, sizeof(EMPTY_SERIAL_STATE));
    uint32_t inputs = modem_inputs();

    unplug_chip();
    detach_translator();
    EXPECT_TRUE(are_sent);
    EXPECT_EQ(inputs, terminal::TIOCM_CTS | terminal::TIOCM_CAR);
}
