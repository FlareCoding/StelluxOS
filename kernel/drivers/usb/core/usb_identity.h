#ifndef STELLUX_DRIVERS_USB_CORE_USB_IDENTITY_H
#define STELLUX_DRIVERS_USB_CORE_USB_IDENTITY_H

#include "drivers/usb/core/usb_device.h"
#include "drivers/usb/usb_descriptors.h"

namespace usb::core {

// The longest location as text: three digits for the controller and each port, the dots between them and the NUL
constexpr size_t LOCATION_TEXT_BYTES = (MAX_PORT_PATH_LENGTH + 1) * 4;

/**
 * Reads the manufacturer, product and serial number strings `desc` names into
 * `dev` and logs them. They are read in the first language the device lists,
 * else US English, and a device whose language table cannot be read is asked
 * nothing more. A string is empty when the device names none, its read fails
 * or its reply holds no text. Blocks on each control transfer it sends, so the
 * caller must be a task that may sleep and must hold no spinlock.
 */
void read_device_strings(device* dev, const usb_device_descriptor& desc);

/**
 * Writes the text of a string descriptor to `out` as printable ASCII, with `?`
 * in place of each UTF-16 code unit outside that range. The text ends at the
 * first NUL, at the descriptor's `bLength`, at the end of its first `length`
 * bytes or at `capacity - 1` characters, whichever comes first. Anything but a
 * string descriptor gives an empty text, and `out` always ends in a NUL.
 * @param length How many bytes of `descriptor` are valid.
 * @param capacity The bytes `out` holds, which must be at least 1.
 */
void decode_string(const uint8_t* descriptor, size_t length, char* out, size_t capacity);

/**
 * The first language ID a language table (string descriptor 0) lists, or US
 * English when the table's first `length` bytes list none.
 */
uint16_t decode_first_language(const uint8_t* table, size_t length);

/**
 * Writes where `dev` is plugged in: its controller's number, its root port,
 * then the port on each hub between them, joined by dots, such as "1.5.3" for
 * port 3 of the hub on root port 5 of controller 1.
 */
void format_location(const device& dev, char (&text)[LOCATION_TEXT_BYTES]);

} // namespace usb::core

#endif // STELLUX_DRIVERS_USB_CORE_USB_IDENTITY_H
