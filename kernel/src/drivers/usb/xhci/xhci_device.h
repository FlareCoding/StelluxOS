#ifndef XHCI_DEVICE_H
#define XHCI_DEVICE_H
#include "xhci_device_ctx.h"

class XhciDeviceEndpointDescriptor {
public:
    XhciDeviceEndpointDescriptor() = default;
    XhciDeviceEndpointDescriptor(uint8_t slotId, UsbEndpointDescriptor* desc);
    ~XhciDeviceEndpointDescriptor();

    uint8_t             slotId;
    uint8_t             endpointNum;
    uint8_t             endpointType;
    uint16_t            maxPacketSize;
    uint8_t             interval;
    uint8_t*            dataBuffer;

    kstl::SharedPtr<XhciTransferRing>  transferRing;
};

class XhciDevice {
public:
    XhciDevice() = default;
    ~XhciDevice() = default;

    uint8_t portRegSet; // Port index of the port register sets (0-based)
    uint8_t portNumber; // Port number (1-based)
    uint8_t speed;      // Speed of the port to which device is connected
    uint8_t slotId;     // Slot index into device context base address array

    // Primary configuration interface
    uint8_t primaryInterface;

    // Device type identification fields
    uint8_t interfaceClass;
    uint8_t interfaceSubClass;
    uint8_t interfaceProtocol;
    
    // Device-specific endpoints specified in the configuration/endpoint descriptors
    kstl::vector<XhciDeviceEndpointDescriptor*> endpoints;
    
    void allocateInputContext(bool use64ByteContexts);
    uint64_t getInputContextPhysicalBase();

    void allocateControlEndpointTransferRing();

    __force_inline__ XhciTransferRing* getControlEndpointTransferRing() { 
        return m_controlEndpointTransferRing.get();
    }

    XhciInputControlContext32* getInputControlContext(bool use64ByteContexts);
    XhciSlotContext32* getInputSlotContext(bool use64ByteContexts);
    XhciEndpointContext32* getInputControlEndpointContext(bool use64ByteContexts);
    XhciEndpointContext32* getInputEndpointContext(bool use64ByteContexts, uint8_t endpointID);

    void copyOutputDeviceContextToInputDeviceContext(bool use64ByteContexts, void* outputDeviceContext);

private:
    void* m_inputContext = nullptr;
    kstl::SharedPtr<XhciTransferRing> m_controlEndpointTransferRing;
};

#endif
