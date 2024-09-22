#ifndef XHCI_REGS_H
#define XHCI_REGS_H

#include "xhci_mem.h"
#include "xhci_ext_cap.h"

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
// xHci Spec Section 5.5.2.1 (page 390)

Address: Runtime Base + 020h + (32 * Interrupter)
        where: Interrupter is 0, 1, 2, 3, … 1023
Default Value: 0000 0000h
Attribute: RW
Size: 32 bits

The Interrupter Management register allows system software to enable,
disable, and detect xHC interrupts.
*/
struct XhciInterrupterManagementRegister {
    union {
        uint32_t raw;
        struct {
            uint32_t interruptPending   : 1;  // Interrupt Pending (IP), bit 0
            uint32_t interruptEnabled   : 1;  // Interrupt Enable (IE), bit 1
            uint32_t reserved           : 30; // Reserved bits, bits 2-31
        } bits __attribute__((packed));
    };
} __attribute__((packed));
static_assert(sizeof(XhciInterrupterManagementRegister) == 4);

/*
// xHci Spec Section 5.5.2 (page 389)

Note: All registers of the Primary Interrupter shall be initialized before
setting the Run/Stop (RS) flag in the USBCMD register to ‘1’. Secondary
Interrupters may be initialized after RS = ‘1’, however all Secondary
Interrupter registers shall be initialized before an event that targets them is
generated. Not following these rules, shall result in undefined xHC behavior.
*/
struct XhciInterrupterRegisters {
    uint32_t iman;         // Interrupter Management (offset 00h)
    uint32_t imod;         // Interrupter Moderation (offset 04h)
    uint32_t erstsz;       // Event Ring Segment Table Size (offset 08h)
    uint32_t rsvdP;        // Reserved (offset 0Ch)
    uint64_t erstba;       // Event Ring Segment Table Base Address (offset 10h)
    union {
        struct {
            // This index is used to accelerate the checking of
            // an Event Ring Full condition. This field can be 0.
            uint64_t dequeueErstSegmentIndex : 3;

            // This bit is set by the controller when it sets the
            // Interrupt Pending bit. Then once your handler is finished
            // handling the event ring, you clear it by writing a '1' to it.
            uint64_t eventHandlerBusy        : 1;

            // Physical address of the _next_ item in the event ring
            uint64_t eventRingDequeuePointer : 60;
        };
        uint64_t erdp;     // Event Ring Dequeue Pointer (offset 18h)
    };
};

/*
// xHci Spec Section 5.5 Table 5-35: Host Controller Runtime Registers (page 388)

This section defines the xHCI Runtime Register space. The base address of this
register space is referred to as Runtime Base. The Runtime Base shall be 32-
byte aligned and is calculated by adding the value Runtime Register Space
Offset register (refer to Section 5.3.8) to the Capability Base address. All
Runtime registers are multiples of 32 bits in length.
Unless otherwise stated, all registers should be accessed with Dword references
on reads, with an appropriate software mask if needed. A software
read/modify/write mechanism should be invoked for partial writes.
Software should write registers containing a Qword address field using only
Qword references. If a system is incapable of issuing Qword references, then 
388 Document Number: 625472, Revision: 1.2b Intel Confidential
writes to the Qword address fields shall be performed using 2 Dword
references; low Dword-first, high-Dword second.
*/
struct XhciRuntimeRegisters {
    uint32_t mfIndex;                          // Microframe Index (offset 0000h)
    uint32_t rsvdZ[7];                         // Reserved (offset 001Fh:0004h)
    XhciInterrupterRegisters ir[1024];         // Interrupter Register Sets (offset 0020h to 8000h)
};

class XhciRuntimeRegisterManager {
public:
    XhciRuntimeRegisterManager(uint64_t base, uint8_t maxInterrupters)
        : m_base(reinterpret_cast<XhciRuntimeRegisters*>(base)),
          m_maxInterrupters(maxInterrupters) {}

    XhciInterrupterRegisters* getInterrupterRegisters(uint8_t interrupter) const;

private:
    XhciRuntimeRegisters*   m_base;
    uint8_t                 m_maxInterrupters;
};

/*
// xHci Spec Section 5.6 Figure 5-29: Doorbell Register (page 394)

The Doorbell Array is organized as an array of up to 256 Doorbell Registers.
One 32-bit Doorbell Register is defined in the array for each Device Slot.
System software utilizes the Doorbell Register to notify the xHC that it has
Device Slot related work for the xHC to perform.
The number of Doorbell Registers implemented by a particular instantiation of a
host controller is documented in the Number of Device Slots (MaxSlots) field of
the HCSPARAMS1 register (section 5.3.3).
These registers are pointed to by the Doorbell Offset Register (DBOFF) in the
xHC Capability register space. The Doorbell Array base address shall be Dword
aligned and is calculated by adding the value in the DBOFF register (section
5.3.7) to “Base” (the base address of the xHCI Capability register address
space).

All registers are 32 bits in length. Software should read and write these
registers using only Dword accesses

Note: Software shall not write the Doorbell of an endpoint until after it has issued a
Configure Endpoint Command for the endpoint and received a successful
Command Completion Event.
*/
struct XhciDoorbellRegister {
    union {
        struct {
            uint8_t     dbTarget;
            uint8_t     rsvd;
            uint16_t    dbStreamId;
        };

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));

class XhciDoorbellManager {
public:
    XhciDoorbellManager(uint64_t base);

    // TargeValue = 2 + (ZeroBasedEndpoint * 2) + (isOutEp ? 0 : 1)
    void ringDoorbell(uint8_t doorbell, uint8_t target);

    void ringCommandDoorbell();
    void ringControlEndpointDoorbell(uint8_t doorbell);

private:
    XhciDoorbellRegister* m_doorbellRegisters;
};

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

class XhciExtendedCapability {
public:
    XhciExtendedCapability(volatile uint32_t* capPtr);

    inline volatile uint32_t* base() const { return m_base; }
    
    inline XhciExtendedCapabilityCode id() const {
        return static_cast<XhciExtendedCapabilityCode>(m_entry.id);
    }

    inline kstl::SharedPtr<XhciExtendedCapability> next() const { return m_next; }

private:
    volatile uint32_t* m_base;
    XhciExtendedCapabilityEntry m_entry;

    kstl::SharedPtr<XhciExtendedCapability> m_next;

private:
    void _readNextExtCaps();
};

const char* xhciExtendedCapabilityToString(XhciExtendedCapabilityCode capid);

/*
// xHci Spec Section 5.4.8 Figure 5-20: Port Status and Control Register (PORTSC) (page 369-370)

Address: Operational Base + (400h + (10h * (n–1)))
where: n = Port Number (Valid values are 1, 2, 3, … MaxPorts)
Default: Field dependent
Attribute: RO, RW, RW1C (field dependent)
Size 32 bits

A host controller shall implement one or more port registers. The number of
port registers implemented by a particular instantiation of a host controller is
documented in the HCSPARAMS1 register (Section 5.3.3). Software uses this 
370 Document Number: 625472, Revision: 1.2b
Intel Confidential
information as an input parameter to determine how many ports need to be
serviced. All ports have the structure defined below.
This register is in the Aux Power well. It is only reset by platform hardware
during a cold reset or in response to a Host Controller Reset (HCRST). The
initial conditions of a port are described in Section 4.19.

Note: Port Status Change Events cannot be generated if the xHC is stopped
(HCHalted (HCH) = ‘1’). Refer to section 4.19.2 for more information about
change flags.

Note: Software shall ensure that the xHC is running (HCHalted (HCH) = ‘0’) before
attempting to write to this register.
Software cannot change the state of the port unless Port Power (PP) is asserted
(‘1’), regardless of the Port Power Control (PPC) capability (section 5.3.6). The
host is required to have power stable to the port within 20 milliseconds of the
‘0’ to ‘1’ transition of PP. If PPC = ‘1’ software is responsible for waiting 20 ms.
after asserting PP, before attempting to change the state of the port.

Note: If a port has been assigned to the Debug Capability, then the port shall not
report device connected (that is, CCS = ‘0’) and enabled when the Port Power
Flag is ‘1’. Refer to Section 7.6 for more information on the xHCI Debug
Capability operation.
*/
struct XhciPortscRegister {
    union {
        struct {
            // Current connect status (RO), if PP is 0, this bit is also 0
            uint32_t    ccs         : 1;

            // Port Enable/Disable (R/WC), if PP is 0, this bit is also 0
            uint32_t    ped         : 1;

            // Reserved and zeroed
            uint32_t    rsvd0       : 1;
            
            // Over-current active (RO)
            uint32_t    oca         : 1;

            // Port reset (R/W), if PP is 0, this bit is also 0
            uint32_t    pr          : 1;

            // Port link state (R/W), if PP is 0, this bit is also 0
            uint32_t    pls         : 4;

            // Port power (R/W)
            uint32_t    pp          : 1;

            // Port speed (RO)
            uint32_t    portSpeed   : 4;

            // Port indicator control (R/W), if PP is 0, this bit is also 0
            uint32_t    pic         : 2;

            // Port link state write strobe (R/W), if PP is 0, this bit is also 0
            uint32_t    lws         : 1;

            // Connect status change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            // ** Clear this bit by writing a '1' to it.
            uint32_t    csc         : 1;

            /*
            Port enable/disable change (R/WC), if PP is 0, this bit is also 0.
            ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            ** For a USB2 protocol port, this bit shall be set to ‘1’ only when the port is disabled (EOF2)
            ** For a USB3 protocol port, this bit shall never be set to ‘1’
            ** Software shall clear this bit by writing a ‘1’ to it. Refer to section 4.19.2
            */
            uint32_t    pec         : 1;

            // Warm port reset change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            // ** Reserved and zeroed on USB2 ports.
            // ** Software shall clear this bit by writing a '1' to it.
            uint32_t    wrc         : 1;

            // Over-current change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            // ** Software shall clear this bit by writing a '1' to it.
            uint32_t    occ         : 1;

            // Port reset change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            // ** Software shall clear this bit by writing a '1' to it.
            uint32_t    prc         : 1;

            // Port link state change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            uint32_t    plc         : 1;

            // Port config error change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            // ** Reserved and zeroed on USB2 ports.
            // ** Software shall clear this bit by writing a '1' to it.
            uint32_t    cec         : 1;

            // Cold attach status (R/O), if PP is 0, this bit is also 0.
            uint32_t    cas         : 1;

            // Wake on connect enable (R/W)
            uint32_t    wce         : 1;

            // Wake on disconnect enable (R/W)
            uint32_t    wde         : 1;

            // Wake on over-current enable (R/W)
            uint32_t    woe         : 1;

            // Reserved and zeroed
            uint32_t    rsvd1        : 2;

            // Device removable (RO)
            uint32_t    dr          : 1;

            // Warm port reset (R/WC).
            // ** Reserved and zeroed on USB2 ports.
            uint32_t    wpr         : 1;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPortscRegister) == sizeof(uint32_t));

// TO-DO add spec page
struct XhciPortpmscRegisterUsb2 {
    union {
        struct {
            uint32_t l1Status                       : 3;
            uint32_t remoteWakeEnable               : 1;
            uint32_t hostInitiatedResumeDuration    : 4;
            uint32_t l1DeviceSlot                   : 8;
            uint32_t hardwareLpmEnable              : 1;
            uint32_t rsvd                           : 11;
            uint32_t portTestControl                : 4;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPortpmscRegisterUsb2) == sizeof(uint32_t));

struct XhciPortpmscRegisterUsb3 {
    union {
        struct {
            uint32_t u1Timeout          : 8;
            uint32_t u2Timeout          : 8;
            uint32_t forceLinkPmAccept  : 1;
            uint32_t rsvd               : 15;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPortpmscRegisterUsb3) == sizeof(uint32_t));

// For USB2.0 this register is reserved and preserved
struct XhciPortliRegister {
    union {
        struct {
            uint32_t linkErrorCount : 16;
            uint32_t rxLaneCount    : 4;
            uint32_t txLaneCount    : 4;
            uint32_t rsvd           : 8;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPortliRegister) == sizeof(uint32_t));

// Port Hardware LPM Control Register
struct XhciPorthlpmcRegisterUsb2 {
    union {
        struct {
            uint32_t hirdm      : 2;
            uint32_t l1Timeout  : 8;
            uint32_t besld      : 4;
            uint32_t rsvd       : 18;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPorthlpmcRegisterUsb2) == sizeof(uint32_t));

struct XhciPorthlpmcRegisterUsb3 {
    union {
        struct {
            uint16_t linkSoftErrorCount;
            uint16_t rsvd;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPorthlpmcRegisterUsb3) == sizeof(uint32_t));

class XhciPortRegisterManager {
public:
    XhciPortRegisterManager(uint64_t base) : m_base(base) {}

    void readPortscReg(XhciPortscRegister& reg) const;
    void writePortscReg(XhciPortscRegister& reg) const;

    void readPortpmscRegUsb2(XhciPortpmscRegisterUsb2& reg) const;
    void writePortpmscRegUsb2(XhciPortpmscRegisterUsb2& reg) const;

    void readPortpmscRegUsb3(XhciPortpmscRegisterUsb3& reg) const;
    void writePortpmscRegUsb3(XhciPortpmscRegisterUsb3& reg) const;


    void readPortliReg(XhciPortliRegister& reg) const;
    void writePortliReg(XhciPortliRegister& reg) const;

    void readPorthlpmcRegUsb2(XhciPorthlpmcRegisterUsb2& reg) const;
    void writePorthlpmcRegUsb2(XhciPorthlpmcRegisterUsb2& reg) const;

    void readPorthlpmcRegUsb3(XhciPorthlpmcRegisterUsb3& reg) const;
    void writePorthlpmcRegUsb3(XhciPorthlpmcRegisterUsb3& reg) const;

private:
    uint64_t m_base;

    const size_t m_portscOffset     = 0x00;
    const size_t m_portpmscOffset   = 0x04;
    const size_t m_portliOffset     = 0x08;
    const size_t m_porthlpmcOffset  = 0x0C;
};

#endif
