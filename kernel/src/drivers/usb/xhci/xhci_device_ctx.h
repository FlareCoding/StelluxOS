#ifndef XHCI_DEVICE_CTX_H
#define XHCI_DEVICE_CTX_H

#include "xhci_rings.h"

/*
// xHci Spec Section 6.2.2 Figure 6-2: Slot Context Data Structure (page 407)

The Slot Context data structure defines information that applies to a device as
a whole.

Note: Figure 6-2 illustrates a 32 byte Slot Context. That is, the Context Size (CSZ)
field in the HCCPARAMS1 register = ‘0’. If the Context Size (CSZ) field = ‘1’
then each Slot Context data structure consumes 64 bytes, where bytes 32 to
63 are also xHCI Reserved (RsvdO).
*/
template <size_t Size>
struct XhciSlotContext {
    union {
        struct {
            /*
                Route String. This field is used by hubs to route packets to the correct downstream port. The
                format of the Route String is defined in section 8.9 the USB3 specification.
                As Input, this field shall be set for all USB devices, irrespective of their speed, to indicate their
                location in the USB topology.
            */
            uint32_t routeString    : 20;

            /*
                Speed. This field is deprecated in this version of the specification and shall be Reserved.
                This field indicates the speed of the device. Refer to the PORTSC Port Speed field in Table 5-27
                for the definition of the valid values.
            */
            uint32_t speed          : 4;

            // Reserved
            uint32_t rz             : 1;

            /*
                Multi-TT (MTT)107. This flag is set to '1' by software if this is a High-speed hub that supports
                Multiple TTs and the Multiple TT Interface has been enabled by software, or if this is a Low-
                /Full-speed device or Full-speed hub and connected to the xHC through a parent108 High-speed
                hub that supports Multiple TTs and the Multiple TT Interface of the parent hub has been
                enabled by software, or ‘0’ if not.
            */
            uint32_t mtt            : 1;

            // Hub. This flag is set to '1' by software if this device is a USB hub, or '0' if it is a USB function.
            uint32_t hub            : 1;

            /*
                Context Entries. This field identifies the index of the last valid Endpoint Context within this
                Device Context structure. The value of ‘0’ is Reserved and is not a valid entry for this field. Valid
                entries for this field shall be in the range of 1-31. This field indicates the size of the Device
                Context structure. For example, ((Context Entries+1) * 32 bytes) = Total bytes for this structure.
                Note, Output Context Entries values are written by the xHC, and Input Context Entries values are
                written by software.
            */
            uint32_t contextEntries : 5;
        };
        uint32_t dword0;
    };

    union {
        struct {
            /*
                Max Exit Latency. The Maximum Exit Latency is in microseconds, and indicates the worst case
                time it takes to wake up all the links in the path to the device, given the current USB link level
                power management settings.
                Refer to section 4.23.5.2 for more information on the use of this field.
            */
            uint16_t    maxExitLatency;
            
            /*
                Root Hub Port Number. This field identifies the Root Hub Port Number used to access the USB
                device. Refer to section 4.19.7 for port numbering information.
                Note: Ports are numbered from 1 to MaxPorts.
            */
            uint8_t     rootHubPortNum;

            /*
                Number of Ports. If this device is a hub (Hub = ‘1’), then this field is set by software to identify
                the number of downstream facing ports supported by the hub. Refer to the bNbrPorts field
                description in the Hub Descriptor (Table 11-13) of the USB2 spec. If this device is not a hub (Hub
                = ‘0’), then this field shall be ‘0’.
            */
            uint8_t     portCount;
        };
        uint32_t dword1;
    };

    union {
        struct {
            /*
                Parent Hub Slot ID. If this device is Low-/Full-speed and connected through a High-speed hub,
                then this field shall contain the Slot ID of the parent High-speed hub109.
                For SS and SSP bus instance, if this device is connected through a higher rank hub110 then this
                field shall contain the Slot ID of the parent hub. For example, a Gen1 x1 connected behind a
                Gen1 x2 hub, or Gen1 x2 device connected behind Gen2 x2 hub.
                This field shall be ‘0’ if any of the following are true:
                    Device is attached to a Root Hub port
                    Device is a High-Speed device
                    Device is the highest rank SS/SSP device supported by xHCI
            */
            uint32_t parentHubSlotId    : 8;
            
            /*
                Parent Port Number. If this device is Low-/Full-speed and connected through a High-speed
                hub, then this field shall contain the number of the downstream facing port of the parent High-
                speed hub109.
                For SS and SSP bus instance, if this device is connected through a higher rank hub110 then this
                field shall contain the number of the downstream facing port of the parent hub. For example, a
                Gen1 x1 connected behind a Gen1 x2 hub, or Gen1 x2 device connected behind Gen2 x2 hub.
                This field shall be ‘0’ if any of the following are true:
                    Device is attached to a Root Hub port
                    Device is a High-Speed device
                    Device is the highest rank SS/SSP device supported by xHCI
            */
            uint32_t parentPortNumber   : 8;

            /*
                TT Think Time (TTT). If this is a High-speed hub (Hub = ‘1’ and Speed = High-Speed), then this
                field shall be set by software to identify the time the TT of the hub requires to proceed to the
                next full-/low-speed transaction.
                Value Think Time
                0 TT requires at most 8 FS bit times of inter-transaction gap on a full-/low-speed
                downstream bus.
                    1 TT requires at most 16 FS bit times.
                    2 TT requires at most 24 FS bit times.
                    3 TT requires at most 32 FS bit times.

                Refer to the TT Think Time sub-field of the wHubCharacteristics field description in the Hub
                Descriptor (Table 11-13) and section 11.18.2 of the USB2 spec for more information on TT
                Think Time. If this device is not a High-speed hub (Hub = ‘0’ or Speed != High-speed), then this
                field shall be ‘0’.
            */
            uint32_t ttThinkTime        : 2;

            // Reserved
            uint32_t rsvd0              : 4;
            
            /*
                Interrupter Target. This field defines the index of the Interrupter that will receive Bandwidth
                Request Events and Device Notification Events generated by this slot, or when a Ring Underrun
                or Ring Overrun condition is reported (refer to section 4.10.3.1). Valid values are between 0 and
                MaxIntrs-1.
            */
            uint32_t interrupterTarget  : 10;
        };
        uint32_t dword2;
    };

    union {
        struct {
            /*
                USB Device Address. This field identifies the address assigned to the USB device by the xHC,
                and is set upon the successful completion of a Set Address Command. Refer to the USB2 spec
                for a more detailed description.
                As Output, this field is invalid if the Slot State = Disabled or Default.
                As Input, software shall initialize the field to ‘0’.
            */
            uint32_t deviceAddress  : 8;

            // Reserved
            uint32_t rsvd1          : 19;
            
            /*
                Slot State. This field is updated by the xHC when a Device Slot transitions from one state to
                another.
                
                Value Slot State
                    0 Disabled/Enabled
                    1 Default
                    2 Addressed
                    3 Configured
                    31-4 Reserved

                Slot States are defined in section 4.5.3.
                As Output, since software initializes all fields of the Device Context data structure to ‘0’, this field
                shall initially indicate the Disabled state.
                As Input, software shall initialize the field to ‘0’.
                Refer to section 4.5.3 for more information on Slot State.
            */
            uint32_t slotState      : 5;
        };
        uint32_t dword3;
    };

    /*
        The remaining bytes (10-1Fh) within the Slot Context are dedicated for exclusive
        use by the xHC and shall be treated by system software as Reserved and Opaque
        (RsvdO).
    */
    uint32_t rsvdZ[4];

    // Padding required for 64-byte context structs
    uint32_t padding[Size == 64 ? 8 : 0];
} __attribute__((packed));

using XhciSlotContext32 = XhciSlotContext<32>;
using XhciSlotContext64 = XhciSlotContext<64>;

// Ensures structure sizes are correct
static_assert(sizeof(XhciSlotContext32) == 32, "32-byte slot context should be 32 bytes");
static_assert(sizeof(XhciSlotContext64) == 64, "64-byte slot context should be 64 bytes");

/*
// xHci Spec Section 6.2.3 Figure 6-3: Endpoint Context Data Structure (page 412)

The Endpoint Context data structure defines information that applies to a
specific endpoint.

Note: Unless otherwise stated: As Input, all fields of the Endpoint Context shall be
initialized to the appropriate value by software before issuing a command. As
Output, the xHC shall update each field to reflect the current value that
it is using.

Note: The remaining bytes (14-1Fh) within the Endpoint Context are dedicated for
exclusive use by the xHC and shall be treated by system software as
Reserved and Opaque (RsvdO).

Note: Figure 6-3 illustrates a 32 byte Endpoint Context. That is, the Context Size
(CSZ) field in the HCCPARAMS1 register = ‘0’. If the Context Size (CSZ) field
= ‘1’ then each Endpoint Context data structure consumes 64 bytes, where
bytes 32 to 63 are xHCI Reserved (RsvdO).
*/
template <size_t Size>
struct XhciEndpointContext {
    union {
        struct {
            /*
                Endpoint State (EP State). The Endpoint State identifies the current operational state of the
                endpoint.
                
                Value Definition
                    0 Disabled The endpoint is not operational
                    1 Running The endpoint is operational, either waiting for a doorbell ring or processing
                    TDs.
                    2 HaltedThe endpoint is halted due to a Halt condition detected on the USB. SW shall issue
                    Reset Endpoint Command to recover from the Halt condition and transition to the Stopped
                    state. SW may manipulate the Transfer Ring while in this state.
                    3 Stopped The endpoint is not running due to a Stop Endpoint Command or recovering
                    from a Halt condition. SW may manipulate the Transfer Ring while in this state.
                    4 Error The endpoint is not running due to a TRB Error. SW may manipulate the Transfer
                    Ring while in this state.
                    5-7 Reserved

                As Output, a Running to Halted transition is forced by the xHC if a STALL condition is detected
                on the endpoint. A Running to Error transition is forced by the xHC if a TRB Error condition is
                detected.
                As Input, this field is initialized to ‘0’ by software.
                Refer to section 4.8.3 for more information on Endpoint State.
            */
            uint32_t endpointState      : 3;

            // Reserved
            uint32_t rsvd0              : 5;

            /*
                Mult. If LEC = ‘0’, then this field indicates the maximum number of bursts within an Interval that
                this endpoint supports. Mult is a “zero-based” value, where 0 to 3 represents 1 to 4 bursts,
                respectively. The valid range of values is ‘0’ to ‘2’. This field shall be ‘0’ for all endpoint types
                except for SS Isochronous.
                If LEC = ‘1’, then this field shall be RsvdZ and Mult is calculated as:
                ROUNDUP(Max ESIT Payload / Max Packet Size / (Max Burst Size + 1)) - 1
            */
            uint32_t mult               : 2;
            
            /*
                Max Primary Streams (MaxPStreams). This field identifies the maximum number of Primary
                Stream IDs this endpoint supports. Valid values are defined below. If the value of this field is ‘0’,
                then the TR Dequeue Pointer field shall point to a Transfer Ring. If this field is > '0' then the TR
                Dequeue Pointer field shall point to a Primary Stream Context Array. Refer to section 4.12 for
                more information.
                A value of ‘0’ indicates that Streams are not supported by this endpoint and the Endpoint
                Context TR Dequeue Pointer field references a Transfer Ring.
                A value of ‘1’ to ‘15’ indicates that the Primary Stream ID Width is MaxPstreams+1 and the
                Primary Stream Array contains 2MaxPStreams+1 entries.
                For SS Bulk endpoints, the range of valid values for this field is defined by the MaxPSASize field
                in the HCCPARAMS1 register (refer to Table 5-13).
                This field shall be '0' for all SS Control, Isoch, and Interrupt endpoints, and for all non-SS
                endpoints
            */
            uint32_t maxPrimaryStreams  : 5;
            
            /*
                Linear Stream Array (LSA). This field identifies how a Stream ID shall be interpreted.
                Setting this bit to a value of ‘1’ shall disable Secondary Stream Arrays and a Stream ID shall be
                interpreted as a linear index into the Primary Stream Array, where valid values for MaxPStreams
                are ‘1’ to ‘15’.
                A value of ‘0’ shall enable Secondary Stream Arrays, where the low order (MaxPStreams+1) bits
                of a Stream ID shall be interpreted as a linear index into the Primary Stream Array, where valid
                values for MaxPStreams are ‘1’ to ‘7’. And the high order bits of a Stream ID shall be interpreted
                as a linear index into the Secondary Stream Array.
                If MaxPStreams = ‘0’, this field RsvdZ.
                Refer to section 4.12.2 for more information.
            */
            uint32_t linearStreamArray  : 1;
            
            /*
                Interval. The period between consecutive requests to a USB endpoint to send or receive data.
                Expressed in 125 μs. increments. The period is calculated as 125 μs. * 2Interval; e.g., an Interval
                value of 0 means a period of 125 μs. (20 = 1 * 125 μs.), a value of 1 means a period of 250 μs. (21
                = 2 * 125 μs.), a value of 4 means a period of 2 ms. (24 = 16 * 125 μs.), etc. Refer to Table 6-12
                for legal Interval field values. See further discussion of this field below. Refer to section 6.2.3.6
                for more information.
            */
            uint32_t interval           : 8;
            
            /*
                Max Endpoint Service Time Interval Payload High (Max ESIT Payload Hi). If LEC = '1', then this
                field indicates the high order 8 bits of the Max ESIT Payload value. If LEC = '0', then this field
                shall be RsvdZ. Refer to section 6.2.3.8 for more information.
            */
            uint32_t maxEsitPayloadHi   : 8;
        };
        uint32_t dword0;
    };

    union {
        struct {
            // Reserved
            uint32_t rsvd1                  : 1;

            /*
                Error Count (CErr)112. This field defines a 2-bit down count, which identifies the number of
                consecutive USB Bus Errors allowed while executing a TD. If this field is programmed with a
                non-zero value when the Endpoint Context is initialized, the xHC loads this value into an internal
                Bus Error Counter before executing a USB transaction and decrements it if the transaction fails.
                If the Bus Error Counter counts from ‘1’ to ‘0’, the xHC ceases execution of the TRB, sets the
                endpoint to the Halted state, and generates a USB Transaction Error Event for the TRB that
                caused the internal Bus Error Counter to decrement to ‘0’. If system software programs this field
                to ‘0’, the xHC shall not count errors for TRBs on the Endpoint’s Transfer Ring and there shall be
                no limit on the number of TRB retries. Refer to section 4.10.2.7 for more information on the
                operation of the Bus Error Counter.
                Note: CErr does not apply to Isoch endpoints and shall be set to ‘0’ if EP Type = Isoch Out ('1') or
                Isoch In ('5').
            */
            uint32_t errorCount             : 2;

            /*
                Endpoint Type (EP Type). This field identifies whether an Endpoint Context is Valid, and if so,
                what type of endpoint the context defines.
                
                Value  Endpoint      Type Direction
                0      Not Valid     N/A
                1      Isoch         Out
                2      Bulk          Out
                3      Interrupt     Out
                4      Control       Bidirectional
                5      Isoch         In
                6      Bulk          In
                7      Interrupt     In
            */
            uint32_t endpointType           : 3;
            
            // Reserved
            uint32_t rsvd2                  : 1;
            
            /*
                Host Initiate Disable (HID). This field affects Stream enabled endpoints, allowing the Host
                Initiated Stream selection feature to be disabled for the endpoint. Setting this bit to a value of
                ‘1’ shall disable the Host Initiated Stream selection feature. A value of ‘0’ will enable normal
                Stream operation. Refer to section 4.12.1.1 for more information.
            */
            uint32_t hostInitiateDisable    : 1;
            
            /*
                Max Burst Size. This field indicates to the xHC the maximum number of consecutive USB
                transactions that should be executed per scheduling opportunity. This is a “zero-based” value,
                where 0 to 15 represents burst sizes of 1 to 16, respectively. Refer to section 6.2.3.4 for more
                information.
            */
            uint32_t maxBurstSize           : 8;
            
            /*
                Max Packet Size. This field indicates the maximum packet size in bytes that this endpoint is
                capable of sending or receiving when configured. Refer to section 6.2.3.5 for more information
            */
            uint32_t maxPacketSize          : 16;
        };
        uint32_t dword1;
    };

    union {
        struct {
            /*
                Dequeue Cycle State (DCS). This bit identifies the value of the xHC Consumer Cycle State (CCS)
                flag for the TRB referenced by the TR Dequeue Pointer. Refer to section 4.9.2 for more
                information. This field shall be ‘0’ if MaxPStreams > ‘0’.
            */
            uint64_t dcs                        : 1;

            // Reserved
            uint64_t rsvd3                      : 3;

            /*
                TR Dequeue Pointer. As Input, this field represents the high order bits of the 64-bit base
                address of a Transfer Ring or a Stream Context Array associated with this endpoint. If
                MaxPStreams = '0' then this field shall point to a Transfer Ring. If MaxPStreams > '0' then this
                field shall point to a Stream Context Array.
                As Output, if MaxPStreams = ‘0’ this field shall be used by the xHC to store the value of the
                Dequeue Pointer when the endpoint enters the Halted or Stopped states, and the value of the
                this field shall be undefined when the endpoint is not in the Halted or Stopped states. if
                MaxPStreams > ‘0’ then this field shall point to a Stream Context Array.
                The memory structure referenced by this physical memory pointer shall be aligned to a 16-byte
                boundary.
            */
            uint64_t trDequeuePtrAddressBits    : 60;
        };
        struct {
            uint32_t dword2;
            uint32_t dword3;
        };
        uint64_t transferRingDequeuePtr;        
    };

    union {
        struct {
            /*
                Average TRB Length. This field represents the average Length of the TRBs executed by this
                endpoint. The value of this field shall be greater than ‘0’. Refer to section 4.14.1.1 and the
                implementation note TRB Lengths and System Bus Bandwidth for more information.
                The xHC shall use this parameter to calculate system bus bandwidth requirements.
            */
            uint16_t averageTrbLength;

            /*
                Max Endpoint Service Time Interval Payload Low (Max ESIT Payload Lo). This field indicates
                the low order 16 bits of the Max ESIT Payload. The Max ESIT Payload represents the total
                number of bytes this endpoint will transfer during an ESIT. This field is only valid for periodic
                endpoints. Refer to section 6.2.3.8 for more information.
            */
            uint16_t maxEsitPayloadLo;
        };
        uint32_t dword4;
    };

    // Padding required for 64-byte context structs
    uint32_t padding[Size == 64 ? 11 : 3];
} __attribute__((packed));

using XhciEndpointContext32 = XhciEndpointContext<32>;
using XhciEndpointContext64 = XhciEndpointContext<64>;

// Ensures structure sizes are correct
static_assert(sizeof(XhciEndpointContext32) == 32, "32-byte endpoint context should be 32 bytes");
static_assert(sizeof(XhciEndpointContext64) == 64, "64-byte endpoint context should be 64 bytes");

/*
// xHci Spec Section 6.2.1 Device Context (page 406)

The Device Context data structure is used in the xHCI architecture as Output
by the xHC to report device configuration and state information to system
software. The Device Context data structure is pointed to by an entry in the
Device Context Base Address Array (refer to section 6.1).
The Device Context Index (DCI) is used to reference the respective element of
the Device Context data structure.
All unused entries of the Device Context shall be initialized to ‘0’ by software.

    Note: Figure 6-1 illustrates offsets with 32-byte Device Context data
    structures. That is, the Context Size (CSZ) field in the HCCPARAMS1 register
    = '0'. If the Context Size (CSZ) field = '1' then the Device Context data
    structures consume 64 bytes each. The offsets shall be 040h for the EP
    Context 0, 080h for EP Context 1, and so on.

    Note: Ownership of the Output Device Context data structure is passed to
    the xHC when software rings the Command Ring doorbell for the first Address
    Device Command issued to a Device Slot after an Enable Slot Command, that
    is, the first transition of the Slot from the Enabled to the Default or Addressed
    state. Software shall initialize the Output Device Context to 0 prior to the
    execution of the first Address Device Command.
*/
template <size_t Size>
struct XhciDeviceContext {
    // Slot context
    XhciSlotContext<Size> slotContext;

    // Primary control endpoint
    XhciEndpointContext<Size> controlEndpointContext;

    // Optional communication endpoints
    XhciEndpointContext<Size> ep[30];
} __attribute__((packed));

using XhciDeviceContext32 = XhciDeviceContext<32>;
using XhciDeviceContext64 = XhciDeviceContext<64>;

// Ensure structure sizes are correct
static_assert(sizeof(XhciDeviceContext32) == 1024, "32-byte device context should be 1024 bytes");
static_assert(sizeof(XhciDeviceContext64) == 2048, "64-byte device context should be 2048 bytes");

/*
// xHci Sped Section 6.2.5.1 Figure 6-6: Input Control Context (page 461)

The Input Control Context data structure defines which Device Context data
structures are affected by a command and the operations to be performed on
those contexts
*/
template <size_t Size>
struct XhciInputControlContext {
    /*
        Drop Context flags (D2 - D31). These single bit fields identify which Device Context data
        structures should be disabled by command. If set to ‘1’, the respective Endpoint Context shall
        be disabled. If cleared to ‘0’, the Endpoint Context is ignored.
    */
    uint32_t dropFlags;

    /*
        Add Context flags (A0 - A31). These single bit fields identify which Device Context data
        structures shall be evaluated and/or enabled by a command. If set to ‘1’, the respective Context
        shall be evaluated. If cleared to ‘0’, the Context is ignored.
    */
    uint32_t addFlags;

    uint32_t rsvd[5];

    /*
        Configuration Value. If CIC = ‘1’, CIE = ‘1’, and this Input Context is associated with a Configure
        Endpoint Command, then this field contains the value of the Standard Configuration Descriptor
        bConfigurationValue field associated with the command, otherwise the this field shall be
        cleared to ‘0’.
    */
    uint8_t configValue;

    /*
        Interface Number. If CIC = ‘1’, CIE = ‘1’, this Input Context is associated with a Configure
        Endpoint Command, and the command was issued due to a SET_INTERFACE request, then this
        field contains the value of the Standard Interface Descriptor bInterfaceNumber field associated
        with the command, otherwise the this field shall be cleared to ‘0’.
    */
    uint8_t interfaceNumber;

    /*
        Alternate Setting. If CIC = ‘1’, CIE = ‘1’, this Input Context is associated with a Configure
        Endpoint Command, and the command was issued due to a SET_INTERFACE request, then this
        field contains the value of the Standard Interface Descriptor bAlternateSetting field associated
        with the command, otherwise the this field shall be cleared to ‘0’
    */
    uint8_t alternateSetting;

    // Reserved and zero'd
    uint8_t rsvdZ;

    // Padding required for 64-byte context structs
    uint32_t padding[Size == 64 ? 8 : 0];
} __attribute__((packed));

using XhciInputControlContext32 = XhciInputControlContext<32>;
using XhciInputControlContext64 = XhciInputControlContext<64>;

// Ensures structure sizes are correct
static_assert(sizeof(XhciInputControlContext32) == 32, "32-byte input control context should be 32 bytes");
static_assert(sizeof(XhciInputControlContext64) == 64, "64-byte input control context should be 64 bytes");

/*
// xHci Sped Section 6.2.5 Input Context (page 459)

The Input Context data structure specifies the endpoints and the operations to
be performed on those endpoints by the Address Device, Configure Endpoint,
and Evaluate Context Commands. Refer to section 4.6 for more information on
these commands.
The Input Context is pointed to by an Input Context Pointer field of an Address
Device, Configure Endpoint, and Evaluate Context Command TRBs. The Input
Context is an array of up to 33 context data structure entries.
*/
template <size_t Size>
struct XhciInputContext {
    XhciInputControlContext<Size>   controlContext;
    XhciDeviceContext<Size>         deviceContext;
};

using XhciInputContext32 = XhciInputContext<32>;
using XhciInputContext64 = XhciInputContext<64>;

// Ensures structure sizes are correct
static_assert(sizeof(XhciInputContext32) == sizeof(XhciInputControlContext32) + sizeof(XhciDeviceContext32), "32-byte input context should be 1056 bytes");
static_assert(sizeof(XhciInputContext64) == sizeof(XhciInputControlContext64) + sizeof(XhciDeviceContext64), "64-byte input context should be 2112 bytes");

class XhciDevice {
public:
    XhciDevice() = default;
    ~XhciDevice() = default;

    uint8_t portRegSet; // Port index of the port register sets (0-based)
    uint8_t portNumber; // Port number (1-based)
    uint8_t speed;      // Speed of the port to which device is connected
    uint8_t slotId;     // Slot index into device context base address array
    
    void allocateInputContext(bool use64ByteContexts);
    uint64_t getInputContextPhysicalBase();

    void allocateControlEndpointTransferRing();

    __force_inline__ XhciTransferRing* getControlEndpointTransferRing() { 
        return m_controlEndpointTransferRing;
    }

    XhciInputControlContext32* getInputControlContext(bool use64ByteContexts);
    XhciSlotContext32* getInputSlotContext(bool use64ByteContexts);
    XhciEndpointContext32* getInputControlEndpointContext(bool use64ByteContexts);

private:
    void* m_inputContext = nullptr;
    XhciTransferRing* m_controlEndpointTransferRing = nullptr;
};

#endif
