#include "xhci_device_ctx.h"
#include <kprint.h>

const char* xhciSlotStateToString(uint8_t slotState) {
    switch (slotState) {
    case XHCI_SLOT_STATE_DISABLED_ENABLED: return "Disabled/Enabled";
    case XHCI_SLOT_STATE_DEFAULT: return "Default";
    case XHCI_SLOT_STATE_ADDRESSED: return "Addressed";
    case XHCI_SLOT_STATE_CONFIGURED: return "Configured";
    case XHCI_SLOT_STATE_RESERVED: return "Reserved";
    default:
        return "Undefined";
    }
}

const char* xhciEndpointStateToString(uint8_t epState) {
    switch (epState) {
    case XHCI_ENDPOINT_STATE_DISABLED: return "Disabled";
    case XHCI_ENDPOINT_STATE_RUNNING: return "Running";
    case XHCI_ENDPOINT_STATE_HALTED: return "Halted";
    case XHCI_ENDPOINT_STATE_STOPPED: return "Stopped";
    case XHCI_ENDPOINT_STATE_ERROR: return "Error";
    default:
        return "Undefined";
    };
}
