#ifndef XHCI_REGS_H
#define XHCI_REGS_H

#include <ktypes.h>
#include "xhci_common.h"

/*
// xHci Spec Section 5.3 Table 5-9: eXtensible Host Controller Capability Registers (page 346)

These registers specify the limits and capabilities of the host controller
implementation.
All Capability Registers are Read-Only (RO). The offsets for these registers are
all relative to the beginning of the host controller’s MMIO address space. The
beginning of the host controller’s MMIO address space is referred to as “Base”.
*/
struct XhciCapabilityRegisters {
    const uint8_t caplength;    // Capability Register Length
    const uint8_t reserved0;
    const uint16_t hciversion;  // Interface Version Number
    const uint32_t hcsparams1;  // Structural Parameters 1
    const uint32_t hcsparams2;  // Structural Parameters 2
    const uint32_t hcsparams3;  // Structural Parameters 3
    const uint32_t hccparams1;  // Capability Parameters 1
    const uint32_t dboff;       // Doorbell Offset
    const uint32_t rtsoff;      // Runtime Register Space Offset
    const uint32_t hccparams2;  // Capability Parameters 2
};
static_assert(sizeof(XhciCapabilityRegisters) == 32);

/*
// xHci Spec Section 5.4 Table 5-18: Host Controller Operational Registers (page 356)

The base address of this register space is referred to as Operational Base.
The Operational Base shall be Dword aligned and is calculated by adding the
value of the Capability Registers Length (CAPLENGTH) register (refer to Section
5.3.1) to the Capability Base address. All registers are multiples of 32 bits in
length.
Unless otherwise stated, all registers should be accessed as a 32-bit width on
reads with an appropriate software mask, if needed. A software
read/modify/write mechanism should be invoked for partial writes.
These registers are located at a positive offset from the Capabilities Registers
(refer to Section 5.3).
*/
struct XhciOperationalRegisters {
    uint32_t usbcmd;        // USB Command
    uint32_t usbsts;        // USB Status
    uint32_t pagesize;      // Page Size
    uint32_t reserved0[2];
    uint32_t dnctrl;        // Device Notification Control
    uint64_t crcr;          // Command Ring Control
    uint32_t reserved1[4];
    uint64_t dcbaap;        // Device Context Base Address Array Pointer
    uint32_t config;        // Configure
    uint32_t reserved2[49];
    // Port Register Set offset has to be calculated dynamically based on MAXPORTS
};
static_assert(sizeof(XhciOperationalRegisters) == 256);

/*
// xHci Spec Section 7.0 Table 7-1: Format of xHCI Extended Capability Pointer Register

The xHC exports xHCI-specific extended capabilities utilizing a method similar to
the PCI extended capabilities. If an xHC implements any extended capabilities, it
specifies a non-zero value in the xHCI Extended Capabilities Pointer (xECP) field
of the HCCPARAMS1 register (5.3.6). This value is an offset into xHC MMIO space
from the Base, where the Base is the beginning of the host controller’s MMIO
address space. Each capability register has the format illustrated in Table 7-1
*/
struct XhciExtendedCapabilityEntry {
    union {
        struct {
            /*
                This field identifies the xHCI Extended capability.
                Refer to Table 7-2 for a list of the valid xHCI extended capabilities.
            */
            uint8_t id;

            /*
                This field points to the xHC MMIO space offset of
                the next xHCI extended capability pointer. A value of 00h indicates the end of the extended
                capability list. A non-zero value in this register indicates a relative offset, in Dwords, from this
                Dword to the beginning of the next extended capability.
                
                For example, assuming an effective address of this data structure is 350h and assuming a
                pointer value of 068h, we can calculate the following effective address:
                350h + (068h << 2) -> 350h + 1A0h -> 4F0h
            */
            uint8_t next;

            /*
                The definition and attributes of these bits depends on the specific capability.
            */
           uint16_t capSpecific;
        };

        // Extended capability entries must be read as 32-bit words
        uint32_t raw;
    };
};
static_assert(sizeof(XhciExtendedCapabilityEntry) == 4);

/*
// xHci Spec Section 7.0 Table 7-1: Format of xHCI Extended Capability Pointer Register
*/
#define XHCI_NEXT_EXT_CAP_PTR(ptr, next) (volatile uint32_t*)((char*)ptr + (next * sizeof(uint32_t)))

/*
// xHci Spec Section 7.0 Table 7-2: xHCI Extended Capability Codes
*/
enum class XhciExtendedCapabilityCode {
    Reserved = 0,
    UsbLegacySupport = 1,
    SupportedProtocol = 2,
    ExtendedPowerManagement = 3,
    IOVirtualizationSupport = 4,
    MessageInterruptSupport = 5,
    LocalMemorySupport = 6,
    UsbDebugCapabilitySupport = 10,
    ExtendedMessageInterruptSupport = 17
};

#endif
