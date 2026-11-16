#include "drivers/usb/serial/ftdi.h"
#include "drivers/usb/serial/bridge.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/usb_descriptors.h"

namespace usb::serial {

static constexpr uint16_t FTDI_VENDOR_ID = 0x0403;
static constexpr uint16_t FT232_PRODUCT_ID = 0x6001;  // FT232AM, FT232BM and FT232R
static constexpr uint16_t FT2232_PRODUCT_ID = 0x6010; // FT2232C, FT2232D and FT2232H
static constexpr uint16_t FT4232H_PRODUCT_ID = 0x6011;
static constexpr uint16_t FT232H_PRODUCT_ID = 0x6014;
static constexpr uint16_t FTX_PRODUCT_ID = 0x6015;    // the FT-X series, such as FT231X

static constexpr uint8_t REQUEST_RESET = 0x00;

// Resets the UART and empties both of its buffers
static constexpr uint16_t RESET_UART = 0x0000;

// Every packet the chip sends starts with its modem status and line status bytes
static constexpr size_t STATUS_BYTES = 2;

static constexpr const char* FAMILY_NAME = "ftdi";

struct chip_release {
    uint16_t    release;
    const char* name;
};

// FTDI chips sharing a product ID differ in the device release number
static constexpr chip_release CHIP_RELEASES[] = {
    {.release = 0x0200, .name = "ft232am"},
    {.release = 0x0400, .name = "ft232bm"},
    {.release = 0x0500, .name = "ft2232c"},
    {.release = 0x0600, .name = "ft232r"},
    {.release = 0x0700, .name = "ft2232h"},
    {.release = 0x0800, .name = "ft4232h"},
    {.release = 0x0900, .name = "ft232h"},
    {.release = 0x1000, .name = "ftx"},
};

// A release the table lacks takes the family's name
static const char* chip_name_for_release(uint16_t release) {
    for (const chip_release& chip : CHIP_RELEASES) {
        if (chip.release == release) {
            return chip.name;
        }
    }

    return FAMILY_NAME;
}

ftdi::ftdi(device* dev, const interface& iface) : translator(dev, iface), m_chip_name(FAMILY_NAME) {}

int32_t ftdi::configure() {
    if (send_request(REQUEST_RESET, RESET_UART) != 0) {
        return ::serial::ERR_IO;
    }

    m_chip_name = chip_name_for_release(m_device->bcd_device);

    return ::serial::OK;
}

const char* ftdi::driver_name() const {
    return FAMILY_NAME;
}

// A chip with several UARTs has an interface for each
int8_t ftdi::channel() const {
    return m_device->num_interfaces > 1 ? static_cast<int8_t>(m_interface->interface_number) : -1;
}

void ftdi::handle_received(::serial::port* p, const uint8_t* data, size_t length, uint16_t packet_size) {
    for (size_t offset = 0; offset < length; offset += packet_size) {
        size_t packet = length - offset < packet_size ? length - offset : packet_size;
        if (packet > STATUS_BYTES) {
            ::serial::receive(p, data + offset + STATUS_BYTES, packet - STATUS_BYTES);
        }
    }
}

int32_t ftdi::send_request(uint8_t request, uint16_t value) {
    // FTDI numbers a chip's UARTs from 1 in each vendor request's index
    uint16_t uart = static_cast<uint16_t>(m_interface->interface_number + 1);
    return control_transfer(
        m_device,
        USB_REQTYPE_DIR_OUT | USB_REQTYPE_TYPE_VENDOR | USB_REQTYPE_RECIP_DEVICE,
        request,
        value,
        uart,
        nullptr,
        0
    );
}

REGISTER_USB_CLASS_DRIVER(ftdi,
    USB_MATCH_DEVICE(FTDI_VENDOR_ID, FT232_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ftdi>);
REGISTER_USB_CLASS_DRIVER(ftdi,
    USB_MATCH_DEVICE(FTDI_VENDOR_ID, FT2232_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ftdi>);
REGISTER_USB_CLASS_DRIVER(ftdi,
    USB_MATCH_DEVICE(FTDI_VENDOR_ID, FT4232H_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ftdi>);
REGISTER_USB_CLASS_DRIVER(ftdi,
    USB_MATCH_DEVICE(FTDI_VENDOR_ID, FT232H_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ftdi>);
REGISTER_USB_CLASS_DRIVER(ftdi,
    USB_MATCH_DEVICE(FTDI_VENDOR_ID, FTX_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ftdi>);

} // namespace usb::serial
