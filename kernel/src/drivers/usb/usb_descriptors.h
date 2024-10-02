#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H
#include <ktypes.h>

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
#define USB_DESCRIPTOR_REPORT                          0x22
#define USB_DESCRIPTOR_PHYSICAL                        0x23

// Hub Descriptor Types
#define USB_DESCRIPTOR_HUB                             0x29
#define USB_DESCRIPTOR_SUPERSPEED_HUB                  0x2A

// Billboarding Descriptor Type
#define USB_DESCRIPTOR_BILLBOARD                       0x0D

// Type-C Bridge Descriptor Type
#define USB_DESCRIPTOR_TYPE_C_BRIDGE                   0x0E

#define USB_DESCRIPTOR_REQUEST(type, index) ((type << 8) | index)

struct UsbDescriptorHeader {
    uint8_t bLength;
    uint8_t bDescriptorType;
} __attribute__((packed));
static_assert(sizeof(UsbDescriptorHeader) == 2);

struct UsbDeviceDescriptor {
    UsbDescriptorHeader header;
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
static_assert(sizeof(UsbDeviceDescriptor) == 18);

struct UsbStringLanguageDescriptor {
    UsbDescriptorHeader header;
    uint16_t langIds[126];
} __attribute__((packed));
static_assert(sizeof(UsbStringLanguageDescriptor) == 254);

struct UsbStringDescriptor {
    UsbDescriptorHeader header;
    uint16_t unicodeString[126];
} __attribute__((packed));
static_assert(sizeof(UsbStringDescriptor) == 254);

struct UsbConfigurationDescriptor {
    UsbDescriptorHeader header;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
    uint8_t data[245];
} __attribute__((packed));
static_assert(sizeof(UsbConfigurationDescriptor) == 254);

struct UsbInterfaceDescriptor {
    UsbDescriptorHeader header;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));
static_assert(sizeof(UsbInterfaceDescriptor) == 9);

struct UsbEndpointDescriptor {
    UsbDescriptorHeader header;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed));
static_assert(sizeof(UsbEndpointDescriptor) == 7);

#endif
