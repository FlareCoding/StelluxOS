#ifndef XHCI_EXT_CAP_H
#define XHCI_EXT_CAP_H
#include <types.h>

/*
// xHci Spec Section 7.2 (page 521)
At least one of these capability structures is required for all xHCI
implementations. More than one may be defined for implementations that
support more than one bus protocol. Refer to section 4.19.7 for more
information.
*/
struct xhci_usb_supported_protocol_capability {
    union {
        struct {
            uint8_t id;
            uint8_t next;
            uint8_t minor_revision_version;
            uint8_t major_revision_version;
        };

        // Extended capability entries must be read as 32-bit words
        uint32_t dword0;
    };

    union {
        uint32_t dword1;
        uint32_t name; // "USB "
    };

    union {
        struct {
            uint8_t compatible_port_offset;
            uint8_t compatible_port_count;
            uint8_t protocol_defined;
            uint8_t protocol_speed_id_count; // (PSIC)
        };

        uint32_t dword2;
    };

    union {
        struct {
            uint32_t slot_type : 4;
            uint32_t reserved : 28;
        };

        uint32_t dword3;
    };

    xhci_usb_supported_protocol_capability() = default;
    xhci_usb_supported_protocol_capability(volatile uint32_t* cap) {
        dword0 = cap[0];
        dword1 = cap[1];
        dword2 = cap[2];
        dword3 = cap[3];
    }
};
static_assert(sizeof(xhci_usb_supported_protocol_capability) == (4 * sizeof(uint32_t)));

#endif // XHCI_EXT_CAP_H
