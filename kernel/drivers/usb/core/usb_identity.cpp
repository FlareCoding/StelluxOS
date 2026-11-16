#include "drivers/usb/core/usb_identity.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/usb_langids.h"
#include "common/logging.h"
#include "common/string.h"

namespace usb::core {

static constexpr size_t STRING_HEADER_BYTES = sizeof(usb_descriptor_header);

// The bytes of a language table up to the end of its first language
static constexpr size_t FIRST_LANGUAGE_BYTES = STRING_HEADER_BYTES + sizeof(uint16_t);

// bLength is one byte, so any string descriptor fits
static constexpr size_t MAX_DESCRIPTOR_BYTES = 255;

static constexpr uint16_t FIRST_PRINTABLE_CHARACTER = 0x20;
static constexpr uint16_t LAST_PRINTABLE_CHARACTER  = 0x7E;
static constexpr char     UNPRINTABLE_REPLACEMENT   = '?';

static int32_t get_string_descriptor(
    device* dev,
    uint8_t index,
    uint16_t language,
    void* out,
    uint16_t length
) {
    return control_transfer(
        dev,
        USB_REQTYPE_DIR_IN | USB_REQTYPE_TYPE_STANDARD | USB_REQTYPE_RECIP_DEVICE,
        USB_REQUEST_GET_DESCRIPTOR,
        USB_DESCRIPTOR_REQUEST(USB_DESCRIPTOR_STRING, index),
        language,
        out,
        length
    );
}

static bool read_first_language(device* dev, uint16_t* language) {
    uint8_t table[FIRST_LANGUAGE_BYTES] = {};
    if (get_string_descriptor(dev, 0, 0, table, sizeof(table)) != 0) {
        return false;
    }

    *language = decode_first_language(table, sizeof(table));
    return true;
}

// Some devices fail a string request for 255 bytes, so the header is read first for the exact length
static void read_string(device* dev, uint8_t index, uint16_t language, char (&out)[MAX_STRING_LENGTH + 1]) {
    if (index == 0) {
        return;
    }

    uint8_t descriptor[MAX_DESCRIPTOR_BYTES] = {};
    if (get_string_descriptor(dev, index, language, descriptor, STRING_HEADER_BYTES) != 0) {
        return;
    }

    uint8_t length = descriptor[0];
    if (length <= STRING_HEADER_BYTES) {
        return;
    }

    if (get_string_descriptor(dev, index, language, descriptor, length) != 0) {
        return;
    }

    decode_string(descriptor, length, out, sizeof(out));
}

// Serial ports are named after their board's serial number, so one the first read misses is read once more
static void read_serial_number(device* dev, uint8_t index, uint16_t language) {
    read_string(dev, index, language, dev->serial_number);
    if (index != 0 && dev->serial_number[0] == '\0') {
        read_string(dev, index, language, dev->serial_number);
    }
}

void read_device_strings(device* dev, const usb_device_descriptor& desc) {
    dev->manufacturer[0] = '\0';
    dev->product[0] = '\0';
    dev->serial_number[0] = '\0';

    uint16_t language = 0;
    bool names_a_string = desc.iManufacturer != 0 || desc.iProduct != 0 || desc.iSerialNumber != 0;
    if (names_a_string && read_first_language(dev, &language)) {
        read_string(dev, desc.iManufacturer, language, dev->manufacturer);
        read_string(dev, desc.iProduct, language, dev->product);
        read_serial_number(dev, desc.iSerialNumber, language);
    }

    if (desc.iSerialNumber != 0 && dev->serial_number[0] == '\0') {
        char location[LOCATION_TEXT_BYTES];
        format_location(*dev, location);
        log::warn("usb: %04x:%04x at location %s names a serial number it did not give", dev->vid, dev->pid, location);
    }

    log::info("usb: %04x:%04x is \"%s\" by \"%s\", release %x.%02x, serial number \"%s\"",
              dev->vid, dev->pid, dev->product, dev->manufacturer,
              dev->bcd_device >> 8, dev->bcd_device & 0xFF, dev->serial_number);
}

void decode_string(const uint8_t* descriptor, size_t length, char* out, size_t capacity) {
    const auto* header = reinterpret_cast<const usb_descriptor_header*>(descriptor);
    size_t text_bytes = 0;

    bool is_string = length >= STRING_HEADER_BYTES && header->bDescriptorType == USB_DESCRIPTOR_STRING;
    if (is_string && header->bLength > STRING_HEADER_BYTES) {
        size_t descriptor_bytes = header->bLength < length ? header->bLength : length;
        text_bytes = descriptor_bytes - STRING_HEADER_BYTES;
    }

    const uint8_t* text = descriptor + STRING_HEADER_BYTES;
    size_t written = 0;
    for (size_t i = 0; i + 1 < text_bytes && written + 1 < capacity; i += 2) {
        uint16_t unit = static_cast<uint16_t>(text[i] | (text[i + 1] << 8));
        if (unit == 0) {
            break;
        }

        bool is_printable = unit >= FIRST_PRINTABLE_CHARACTER && unit <= LAST_PRINTABLE_CHARACTER;
        out[written++] = is_printable ? static_cast<char>(unit) : UNPRINTABLE_REPLACEMENT;
    }

    out[written] = '\0';
}

uint16_t decode_first_language(const uint8_t* table, size_t length) {
    const auto* header = reinterpret_cast<const usb_descriptor_header*>(table);

    bool lists_a_language = length >= FIRST_LANGUAGE_BYTES &&
                            header->bLength >= FIRST_LANGUAGE_BYTES &&
                            header->bDescriptorType == USB_DESCRIPTOR_STRING;
    if (!lists_a_language) {
        return USB_LANGID_ENGLISH_US;
    }

    const uint8_t* language = table + STRING_HEADER_BYTES;
    return static_cast<uint16_t>(language[0] | (language[1] << 8));
}

void format_location(const device& dev, char (&text)[LOCATION_TEXT_BYTES]) {
    size_t length = string::format_u64(text, sizeof(text) - 1, dev.controller_number);
    for (uint8_t i = 0; i < dev.port_path_length; i++) {
        text[length++] = '.';
        length += string::format_u64(&text[length], sizeof(text) - 1 - length, dev.port_path[i]);
    }

    text[length] = '\0';
}

} // namespace usb::core
