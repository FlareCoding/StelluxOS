#include <drivers/usb/xhci/xhci_rings.h>

const char* xhci_slot_state_to_string(uint8_t slot_state) {
    switch (slot_state) {
    case XHCI_SLOT_STATE_DISABLED_ENABLED: return "Disabled/Enabled";
    case XHCI_SLOT_STATE_DEFAULT: return "Default";
    case XHCI_SLOT_STATE_ADDRESSED: return "Addressed";
    case XHCI_SLOT_STATE_CONFIGURED: return "Configured";
    case XHCI_SLOT_STATE_RESERVED: return "Reserved";
    default:
        return "Undefined";
    }
}

const char* xhci_ep_state_to_string(uint8_t ep_state) {
    switch (ep_state) {
    case XHCI_ENDPOINT_STATE_DISABLED: return "Disabled";
    case XHCI_ENDPOINT_STATE_RUNNING: return "Running";
    case XHCI_ENDPOINT_STATE_HALTED: return "Halted";
    case XHCI_ENDPOINT_STATE_STOPPED: return "Stopped";
    case XHCI_ENDPOINT_STATE_ERROR: return "Error";
    default:
        return "Undefined";
    };
}
