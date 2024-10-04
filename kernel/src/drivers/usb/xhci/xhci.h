#ifndef XCHI_H
#define XCHI_H
#include <drivers/device_driver.h>
#include <interrupts/interrupts.h>
#include "xhci_device_ctx.h"

/*
// xHci Spec Section 4.2 Host Controller Initialization (page 68)

When the system boots, the host controller is enumerated, assigned a base
address for the xHC register space, and the system software sets the Frame
Length Adjustment (FLADJ) register to a system-specific value.
Refer to Section 4.23.1 Power Wells for a discussion of the effect of Power
Wells on register state after power-on and light resets.
Document Number: 625472, Revision: 1.2b 69
Following are a review of the operations that system software would perform in
order to initialize the xHC using MSI-X as the interrupt mechanism:
    • Initialize the system I/O memory maps, if supported.
    
    • After Chip Hardware Reset6 wait until the Controller Not Ready (CNR) flag
in the USBSTS is ‘0’ before writing any xHC Operational or Runtime
registers.

Note: This text does not imply a specific order for the following operations, however
these operations shall be completed before setting the USBCMD register
Run/Stop (R/S) bit to ‘1’.


    • Program the Max Device Slots Enabled (MaxSlotsEn) field in the CONFIG
register (5.4.7) to enable the device slots that system software is going to
use.

    • Program the Device Context Base Address Array Pointer (DCBAAP) register
(Section 5.4.6 Device Context Base Address Array Pointer Register
(DCBAAP)) with a 64-bit address pointing to where the Device Context
Base Address Array is located.

    • Define the Command Ring Dequeue Pointer by programming the Command
Ring Control Register (Section 5.4.5 Command Ring Control Register
(CRCR)) with a 64-bit address pointing to the starting address of the first
TRB of the Command Ring.

    • Initialize interrupts by:
        o Allocate and initialize the MSI-X Message Table (Section 5.2.8.3 MSI-X
        Table), setting the Message Address and Message Data, and enable the
        vectors. At a minimum, table vector entry 0 shall be initialized and
        enabled. Refer to the PCI specification for more details.
        o Allocate and initialize the MSI-X Pending Bit Array (PBA, Section 5.2.8.4
        MSI-X PBA).
        o Point the Table Offset and PBA Offsets in the MSI-X Capability Structure
        to the MSI-X Message Control Table and Pending Bit Array,
        respectively.
        o Initialize the Message Control register (Section 5.2.8.3 MSI-X Table) of
        the MSI-X Capability Structure.
        o Initialize each active interrupter by:
            ▪ Defining the Event Ring: (refer to Section 4.9.4 Event Ring
            Management for a discussion of Event Ring Management.)

    • Allocate and initialize the Event Ring Segment(s).
Refer to the PCI spec for the initialization and use of MSI or PIN interrupt mechanisms
A Chip Hardware Reset may be either a PCI reset input or an optional power-on reset input to the xHC.

Interrupts are optional. The xHC may be managed by polling Event Rings.
Document Number: 625472, Revision: 1.2b Intel Confidential

    • Allocate the Event Ring Segment Table (ERST) (Section 6.5
Event Ring Segment Table). Initialize ERST table entries to
point to and to define the size (in TRBs) of the respective Event
Ring Segment.

    • Program the Interrupter Event Ring Segment Table Size
(ERSTSZ) register (Section 5.5.2.3.1 Event Ring Segment Table
Size Register (ERSTSZ)) with the number of segments
described by the Event Ring Segment Table.

    • Program the Interrupter Event Ring Dequeue Pointer (ERDP)
register (Section 5.5.2.3.3 Event Ring Dequeue Pointer Register
(ERDP)) with the starting address of the first segment
described by the Event Ring Segment Table.

    • Program the Interrupter Event Ring Segment Table Base
Address (ERSTBA) register (Section 5.5.2.3.2 Event Ring
Segment Table Base Address Register (ERSTBA)) with a 64-bit
address pointer to where the Event Ring Segment Table is
located.

Note that writing the ERSTBA enables the Event Ring. Refer to
Section 4.9.4 Event Ring Management for more information on
the Event Ring registers and their initialization.

    ▪ Defining the interrupts:
        • Enable the MSI-X interrupt mechanism by setting the MSI-X
        Enable flag in the MSI-X Capability Structure Message Control
        register (5.2.8.3).
        • Initializing the Interval field of the Interrupt Moderation register
        (5.5.2.2) with the target interrupt moderation rate.
        • Enable system bus interrupt generation by writing a ‘1’ to the
        Interrupter Enable (INTE) flag of the USBCMD register (5.4.1).
        • Enable the Interrupter by writing a ‘1’ to the Interrupt Enable
        (IE) field of the Interrupter Management register (5.5.2.1).
        • Write the USBCMD (5.4.1) to turn the host controller ON via setting the
        Run/Stop (R/S) bit to ‘1’. This operation allows the xHC to begin accepting
        doorbell references.
*/
class XhciDriver : public DeviceDriver {
public:
    XhciDriver() = default;
    ~XhciDriver() = default;

    int driverInit(PciDeviceInfo& pciInfo, uint8_t irqVector);

    void logUsbsts();

    static irqreturn_t xhciIrqHandler(void*, XhciDriver* driver);

private:
    uint64_t m_xhcBase;

    volatile XhciCapabilityRegisters* m_capRegs;
    volatile XhciOperationalRegisters* m_opRegs;

    void _parseCapabilityRegisters();
    void _logCapabilityRegisters();

    void _parseExtendedCapabilityRegisters();

    void _configureOperationalRegisters();
    void _logOperationalRegisters();
    
    uint8_t _getPortSpeed(uint8_t port);
    const char* _usbSpeedToString(uint8_t speed); 

    void _configureRuntimeRegisters();

    bool _isUSB3Port(uint8_t portNum);
    XhciPortRegisterManager _getPortRegisterSet(uint8_t portNum);

    void _setupDcbaa();

    // Creates a device context buffer and inserts it into DCBAA
    void _createDeviceContext(uint8_t slotId);

    XhciCommandCompletionTrb_t* _sendCommand(XhciTrb_t* trb, uint32_t timeoutMs = 120);
    XhciTransferCompletionTrb_t* _startControlEndpointTransfer(XhciTransferRing* transferRing);

    uint16_t _getMaxInitialPacketSize(uint8_t portSpeed);

private:
    void _processEvents();
    void _acknowledgeIrq(uint8_t interrupter);

    bool _resetHostController();
    void _startHostController();

    // Reset a 0-indexed port number
    bool _resetPort(uint8_t portNum);
    uint8_t _enableDeviceSlot();
    void _configureDeviceInputContext(XhciDevice* device, uint16_t maxPacketSize);

    void _configureDeviceInterruptEndpoint(XhciDevice* device, UsbEndpointDescriptor* epDesc);

    void _setupDevice(uint8_t port);
    bool _addressDevice(XhciDevice* device, bool bsr);
    bool _configureEndpoint(XhciDevice* device);
    bool _evaluateContext(XhciDevice* device);

    bool _sendUsbRequestPacket(XhciDevice* device, XhciDeviceRequestPacket& req, void* outputBuffer, uint32_t length);

    bool _getDeviceDescriptor(XhciDevice* device, UsbDeviceDescriptor* desc, uint32_t length);
    bool _getStringLanguageDescriptor(XhciDevice* device, UsbStringLanguageDescriptor* desc);
    bool _getStringDescriptor(XhciDevice* device, uint8_t descriptorIndex, uint8_t langid, UsbStringDescriptor* desc);
    bool _getConfigurationDescriptor(XhciDevice* device, UsbConfigurationDescriptor* desc);

    bool _setDeviceConfiguration(XhciDevice* device, uint16_t configurationValue);
    bool _setProtocol(XhciDevice* device, uint8_t interface, uint8_t protocol);

    void _testKeyboardCommunication(XhciDevice* device);
    char _mapKeycodeToChar(uint8_t keycode, bool shiftPressed);

private:
    // CAPLENGTH
    uint8_t m_capabilityRegsLength;
    
    // HCSPARAMS1
    uint8_t m_maxDeviceSlots;
    uint8_t m_maxInterrupters;
    uint8_t m_maxPorts;

    // HCSPARAMS2
    uint8_t m_isochronousSchedulingThreshold;
    uint8_t m_erstMax;
    uint8_t m_maxScratchpadBuffers;

    // HCCPARAMS1
    bool m_64bitAddressingCapability;
    bool m_bandwidthNegotiationCapability;
    bool m_64ByteContextSize;
    bool m_portPowerControl;
    bool m_portIndicators;
    bool m_lightResetCapability;
    uint32_t m_extendedCapabilitiesOffset;

    // Linked list of extended capabilities
    kstl::SharedPtr<XhciExtendedCapability> m_extendedCapabilitiesHead;

    // Page size supported by host controller
    uint64_t m_hcPageSize;

    // USB3.x-specific ports
    kstl::vector<uint8_t> m_usb3Ports;

    // Device context base address array's virtual address
    uint64_t* m_dcbaa;

    // Controller class for runtime registers
    kstl::SharedPtr<XhciRuntimeRegisterManager> m_runtimeRegisterManager;

    // Main command ring
    kstl::SharedPtr<XhciCommandRing> m_commandRing;

    // Main event ring
    kstl::SharedPtr<XhciEventRing> m_eventRing;

    // Doorbell register array manager
    kstl::SharedPtr<XhciDoorbellManager> m_doorbellManager;

    // Test device
    XhciDevice* keyboardDevice;
    uint8_t* keyboardDeviceDataBuffer;

private:
    kstl::vector<XhciPortStatusChangeTrb_t*> m_portStatusChangeEvents;
    kstl::vector<XhciCommandCompletionTrb_t*> m_commandCompletionEvents;
    kstl::vector<XhciTransferCompletionTrb_t*> m_transferCompletionEvents;

    volatile uint8_t m_commandIrqCompleted = 0;
    volatile uint8_t m_transferIrqCompleted = 0;
};

#endif
