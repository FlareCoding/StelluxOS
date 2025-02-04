#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H
#include <types.h>

// USB Standard Descriptor Types
#define USB_DESCRIPTOR_DEVICE                          0x01
#define USB_DESCRIPTOR_CONFIGURATION                   0x02
#define USB_DESCRIPTOR_STRING                          0x03
#define USB_DESCRIPTOR_INTERFACE                       0x04
#define USB_DESCRIPTOR_ENDPOINT                        0x05
#define USB_DESCRIPTOR_DEVICE_QUALIFIER                0x06
#define USB_DESCRIPTOR_OTHER_SPEED_CONFIGURATION       0x07
#define USB_DESCRIPTOR_INTERFACE_POWER                 0x08
#define USB_DESCRIPTOR_OTG                             0x09
#define USB_DESCRIPTOR_DEBUG                           0x0A
#define USB_DESCRIPTOR_INTERFACE_ASSOCIATION           0x0B
#define USB_DESCRIPTOR_BOS                             0x0F
#define USB_DESCRIPTOR_DEVICE_CAPABILITY               0x10
#define USB_DESCRIPTOR_WIRELESS_ENDPOINT_COMPANION     0x11
#define USB_DESCRIPTOR_SUPERSPEED_ENDPOINT_COMPANION   0x30
#define USB_DESCRIPTOR_SUPERSPEEDPLUS_ISO_ENDPOINT_COMPANION 0x31

// HID Class-Specific Descriptor Types
#define USB_DESCRIPTOR_HID                             0x21
#define USB_DESCRIPTOR_HID_REPORT                      0x22
#define USB_DESCRIPTOR_HID_PHYSICAL_REPORT             0x23

// Hub Descriptor Types
#define USB_DESCRIPTOR_HUB                             0x29
#define USB_DESCRIPTOR_SUPERSPEED_HUB                  0x2A

// Billboarding Descriptor Type
#define USB_DESCRIPTOR_BILLBOARD                       0x0D

// Type-C Bridge Descriptor Type
#define USB_DESCRIPTOR_TYPE_C_BRIDGE                   0x0E

#define USB_DESCRIPTOR_REQUEST(type, index) ((type << 8) | index)

struct usb_descriptor_header {
    uint8_t bLength;
    uint8_t bDescriptorType;
} __attribute__((packed));
static_assert(sizeof(usb_descriptor_header) == 2);

struct usb_device_descriptor {
    usb_descriptor_header header;
    uint16_t bcdUsb;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed));
static_assert(sizeof(usb_device_descriptor) == 18);

struct usb_string_language_descriptor {
    usb_descriptor_header header;
    uint16_t lang_ids[126];
} __attribute__((packed));
static_assert(sizeof(usb_string_language_descriptor) == 254);

struct usb_string_descriptor {
    usb_descriptor_header header;
    uint16_t unicode_string[126];
} __attribute__((packed));
static_assert(sizeof(usb_string_descriptor) == 254);

struct usb_configuration_descriptor {
    usb_descriptor_header header;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
    uint8_t data[245];
} __attribute__((packed));
static_assert(sizeof(usb_configuration_descriptor) == 254);

struct usb_interface_descriptor {
    usb_descriptor_header header;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));
static_assert(sizeof(usb_interface_descriptor) == 9);

struct usb_hid_descriptor {
    usb_descriptor_header header;
    uint16_t bcdHID;
    uint8_t  bCountryCode;
    uint8_t  bNumDescriptors;
    struct {
        uint8_t  bDescriptorType;
        uint16_t wDescriptorLength;
    } __attribute__((packed)) desc[1];
} __attribute__((packed));
static_assert(sizeof(usb_hid_descriptor) == 9);

struct usb_endpoint_descriptor {
    usb_descriptor_header header;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed));
static_assert(sizeof(usb_endpoint_descriptor) == 7);

#endif
