#ifndef XHCI_TRB_H
#define XHCI_TRB_H

#include "xhci_common.h"

/*
// xHci Spec Section 4.11 Figure 4-13: TRB Template (page 188)

This section discusses the properties and uses of TRBs that are outside of the
scope of the general data structure descriptions that are provided in section
6.4.
*/
typedef struct XhciTransferRequestBlock {
    uint64_t parameter; // TRB-specific parameter
    uint32_t status;    // Status information
    union {
        struct {
            uint32_t cycleBit               : 1;
            uint32_t evalNextTrb            : 1;
            uint32_t interruptOnShortPkt    : 1;
            uint32_t noSnoop                : 1;
            uint32_t chainBit               : 1;
            uint32_t interruptOnCompletion  : 1;
            uint32_t immediateData          : 1;
            uint32_t rsvd0                  : 2;
            uint32_t blockEventInterrupt    : 1;
            uint32_t trbType                : 6;
            uint32_t rsvd1                  : 16;
        };
        uint32_t control;   // Control bits, including the TRB type
    };
} XhciTrb_t;
static_assert(sizeof(XhciTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciAddressDeviceRequestBlock {
    uint64_t inputContextPhysicalBase;
    uint32_t rsvd;
    struct {
        uint32_t cycleBit   : 1;
        uint32_t rsvd1      : 8;

        /*
            Block Set Address Request (BSR). When this flag is set to ‘0’ the Address Device Command shall
            generate a USB SET_ADDRESS request to the device. When this flag is set to ‘1’ the Address
            Device Command shall not generate a USB SET_ADDRESS request. Refer to section 4.6.5 for
            more information on the use of this flag.
        */
        uint32_t bsr        : 1; // Block Set Address Request bit
        
        uint32_t trbType    : 6;
        uint32_t rsvd2      : 8;
        uint32_t slotId     : 8;
    };
} XhciAddressDeviceCommandTrb_t;
static_assert(sizeof(XhciAddressDeviceCommandTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciEvaluateContextCommandRequestBlock {
    uint64_t inputContextPhysicalBase;
    uint32_t rsvd0;
    struct {
        uint32_t cycleBit   : 1;
        uint32_t rsvd1      : 8;
        uint32_t rsvd2      : 1; // Block Set Address Request bit in the Address Device TRB
        uint32_t trbType    : 6;
        uint32_t rsvd3      : 8;
        uint32_t slotId     : 8;
    };
} XhciEvaluateContextCommandTrb_t;
static_assert(sizeof(XhciEvaluateContextCommandTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciConfigureEndpointCommandRequestBlock {
    uint64_t inputContextPhysicalBase;
    uint32_t rsvd0;
    struct {
        uint32_t cycleBit    : 1;
        uint32_t rsvd1       : 8;
        uint32_t deconfigure : 1;
        uint32_t trbType     : 6;
        uint32_t rsvd3       : 8;
        uint32_t slotId      : 8;
    };
} XhciConfigureEndpointCommandTrb_t;
static_assert(sizeof(XhciConfigureEndpointCommandTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciCommandCompletionRequestBlock {
    uint64_t commandTrbPointer;
    struct {
        uint32_t rsvd0          : 24;
        uint32_t completionCode : 8;
    };
    struct {
        uint32_t cycleBit   : 1;
        uint32_t rsvd1      : 9;
        uint32_t trbType    : 6;
        uint32_t vfid       : 8;
        uint32_t slotId     : 8;
    };
} XhciCommandCompletionTrb_t;
static_assert(sizeof(XhciCommandCompletionTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciTransferCompletionRequestBlock {
    uint64_t transferTrbPointer;
    struct {
        uint32_t transferLength : 24;
        uint32_t completionCode : 8;
    };
    struct {
        uint32_t cycleBit   : 1;
        uint32_t rsvd1      : 1;
        uint32_t eventData  : 1;
        uint32_t rsvd2      : 7;
        uint32_t trbType    : 6;
        uint32_t endpointId : 5;
        uint32_t rsvd3      : 3;
        uint32_t slotId     : 8;
    };
} XhciTransferCompletionTrb_t;
static_assert(sizeof(XhciTransferCompletionTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciSetupDataStageCompletionRequestBlock {
    uint64_t commandTrbPointer;
    struct {
        uint32_t bytesTransfered    : 24;
        uint32_t completionCode     : 8;
    };
    struct {
        uint32_t cycleBit   : 1;
        uint32_t rsvd1      : 9;
        uint32_t trbType    : 6;
        uint32_t vfid       : 8;
        uint32_t slotId     : 8;
    };
} XhciSetupDataStageCompletionTrb_t;
static_assert(sizeof(XhciSetupDataStageCompletionTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciPortStatusChangeRequestBlock {
    struct {
        uint32_t rsvd0  : 24;
        uint32_t portId : 8;
    };
    uint32_t rsvd1;
    struct {
        uint32_t rsvd2          : 24;
        uint32_t completionCode : 8;
    };
    struct {
        uint32_t cycleBit  : 1;
        uint32_t rsvd3     : 9;
        uint32_t trbType   : 6;
        uint32_t rsvd4     : 16;
    };
} XhciPortStatusChangeTrb_t;
static_assert(sizeof(XhciPortStatusChangeTrb_t) == sizeof(uint32_t) * 4);

/*
// xHci Spec Section 4.11.2.2 Figure 4-14 SETUP Data, the Parameter Component of Setup Stage TRB (page 211)
*/
struct XhciDeviceRequestPacket {
    union {
        struct {
            /*
                Recipient Values:
                0       = Device
                1       = Interface
                2       = Endpoint
                3       = Other
                4..31   = Reserved
            */
            uint8_t recipient           : 5;

            /*
                Type Values:
                0       = Standard
                1       = Class
                2       = Vendor
                3       = Reserved
            */
            uint8_t type                : 2;

            /*
                Direction Values:
                0       = Host to Device
                1       = Device to Host
            */
            uint8_t transferDirection   : 1;
        };

        uint8_t bRequestType;
    };

    // Desired request
    uint8_t bRequest;

    // Request-specific
    uint16_t wValue;

    // Request-specific
    uint16_t wIndex;

    // Number of bytes to transfer if in data phase
    uint16_t wLength;
};
static_assert(sizeof(XhciDeviceRequestPacket) == 8);

/*
// xHci Spec Section 6.4.1.2.1 Setup Stage TRB (page 468)

A Setup Stage TRB is created by system software to initiate a USB Setup packet
on a control endpoint. Refer to section 3.2.9 for more information on Setup
Stage TRBs and the operation of control endpoints. Also refer to section 8.5.3 in
the USB2 spec. for a description of “Control Transfers”.
*/
typedef struct XhciSetupStageTransferRequestBlock {
    XhciDeviceRequestPacket requestPacket;
    
    struct {
        // Always 8
        uint32_t trbTransferLength  : 17;

        // Reserved
        uint32_t rsvd0              : 5;

        /*
            This field defines the index of the Interrupter that will receive events
            generated by this TRB. Valid values are between 0 and MaxIntrs-1.
        */
        uint32_t interrupterTarget  : 10;
    };

    struct {
        // This bit is used to mark the Enqueue point of a Transfer ring
        uint32_t cycleBit       : 1;

        // Reserved
        uint32_t rsvd1          : 4;

        /*
            Interrupt On Completion (IOC). If this bit is set to ‘1’, it specifies that when this TRB
            completes, the Host Controller shall notify the system of the completion by placing an
            Event TRB on the Event ring and sending an interrupt at the next interrupt threshold.
            Refer to section 4.10.4.
        */
        uint32_t ioc             : 1;

        /*
            Immediate Data (IDT). This bit shall be set to ‘1’ in a Setup Stage TRB.
            It specifies that the Parameter component of this TRB contains Setup Data.
        */
        uint32_t idt            : 1;

        // Reserved
        uint32_t rsvd2          : 3;

        /*
            TRB Type. This field is set to Setup Stage TRB type.
            Refer to Table 6-91 for the definition of the Type TRB IDs.
        */
        uint32_t trbType        : 6;

        /*
            Transfer Type (TRT). This field indicates the type and direction of the control transfer.
            Value Definition
            0 No Data Stage
            1 Reserved
            2 OUT Data Stage
            3 IN Data Stage
            Refer to section 4.11.2.2 for more information on the use of TRT.
        */
        uint32_t trt            : 2;

        // Reserved
        uint32_t rsvd3          : 14;
    };
} XhciSetupStageTrb_t;
static_assert(sizeof(XhciSetupStageTrb_t) == sizeof(uint32_t) * 4);

/*
// xHci Spec Section 6.4.1.2.2 Data Stage TRB Figure 6-10: Data Stage TRB (page 470)

A Data Stage TRB is used generate the Data stage transaction of a USB Control
transfer. Refer to section 3.2.9 for more information on Control transfers and
the operation of control endpoints. Also refer to section 8.5.3 in the USB2 spec.
for a description of “Control Transfers”.
*/
typedef struct XhciDataStageTransferRequestBlock {
    /*
        Data Buffer Pointer Hi and Lo. These fields represent the 64-bit address of the Data
        buffer area for this transaction.

        The memory structure referenced by this physical memory pointer is allowed to begin on
        a byte address boundary. However, user may find other alignments, such as 64-byte or
        128-byte alignments, to be more efficient and provide better performance.
    */
    uint64_t dataBuffer;

    struct {
        /*
            TRB Transfer Length. For an OUT, this field is the number of data bytes the xHC will send
            during the execution of this TRB.

            For an IN, the initial value of the field identifies the size of the data buffer referenced
            by the Data Buffer Pointer, i.e. the number of bytes the host expects the endpoint to deliver.
            Valid values are 1 to 64K.
        */
        uint32_t trbTransferLength  : 17;

        /*
            TD Size. This field provides an indicator of the number of packets remaining in the TD.
            Refer to section 4.11.2.4 for how this value is calculated.
        */
        uint32_t tdSize             : 5;

        /*
            This field defines the index of the Interrupter that will receive events
            generated by this TRB. Valid values are between 0 and MaxIntrs-1.
        */
        uint32_t interrupterTarget  : 10;
    };

    struct {
        // This bit is used to mark the Enqueue point of a Transfer ring
        uint32_t cycleBit       : 1;

        /*
            Evaluate Next TRB (ENT). If this flag is ‘1’ the xHC shall fetch and evaluate the
            next TRB before saving the endpoint state. Refer to section 4.12.3 for more information.
        */
        uint32_t ent            : 1;

        /*
            Interrupt-on Short Packet (ISP). If this flag is ‘1’ and a Short Packet is encountered
            for this TRB (i.e., less than the amount specified in TRB Transfer Length), then a
            Transfer Event TRB shall be generated with its Completion Code set to Short Packet.
            The TRB Transfer Length field in the Transfer Event TRB shall reflect the residual
            number of bytes not transferred into the associated data buffer. In either case, when
            a Short Packet is encountered, the TRB shall be retired without error and the xHC shall
            advance to the Status Stage TD.

            Note: if the ISP and IOC flags are both ‘1’ and a Short Packet is detected, then only one
            Transfer Event TRB shall be queued to the Event Ring. Also refer to section 4.10.1.1.
        */
        uint32_t isp            : 1;

        /*
            No Snoop (NS). When set to ‘1’, the xHC is permitted to set the No Snoop bit in the
            Requester Attributes of the PCIe transactions it initiates if the PCIe configuration
            Enable No Snoop flag is also set.
            
            When cleared to ‘0’, the xHC is not permitted to set PCIe packet No Snoop Requester
            Attribute. Refer to section 4.18.1 for more information.

            NOTE: If software sets this bit, then it is responsible for maintaining cache consistency.
        */
        uint32_t noSnoop        : 1;

        /*
            Chain bit (CH). Set to ‘1’ by software to associate this TRB with the next TRB on the Ring.
            A Data Stage TD is defined as a Data Stage TRB followed by zero or more Normal TRBs.
            The Chain bit is used to identify a multi-TRB Data Stage TD.
            
            The Chain bit is always ‘0’ in the last TRB of a Data Stage TD.
        */
        uint32_t chain          : 1;

        /*
            Interrupt On Completion (IOC). If this bit is set to ‘1’, it specifies that when this TRB
            completes, the Host Controller shall notify the system of the completion by placing an
            Event TRB on the Event ring and sending an interrupt at the next interrupt threshold.
            Refer to section 4.10.4.
        */
        uint32_t ioc             : 1;

        /*
            Immediate Data (IDT). This bit shall be set to ‘1’ in a Setup Stage TRB.
            It specifies that the Parameter component of this TRB contains Setup Data.
        */
        uint32_t idt            : 1;

        // Reserved
        uint32_t rsvd0          : 3;

        /*
            TRB Type. This field is set to Setup Stage TRB type.
            Refer to Table 6-91 for the definition of the Type TRB IDs.
        */
        uint32_t trbType        : 6;

        /*
            Direction (DIR). This bit indicates the direction of the data transfer as defined in the
            Data State TRB Direction column of Table 7. If cleared to ‘0’, the data stage transfer
            direction is OUT (Write Data).
            
            If set to ‘1’, the data stage transfer direction is IN (Read Data).
            Refer to section 4.11.2.2 for more information on the use of DIR.
        */
        uint32_t dir            : 1;

        // Reserved
        uint32_t rsvd1          : 15;
    };
} XhciDataStageTrb_t;
static_assert(sizeof(XhciDataStageTrb_t) == sizeof(uint32_t) * 4);

/*
// xHci Spec Section 6.4.1.2.3 Status Stage TRB Figure 6-11: Status Stage TRB (page 472).

A Status Stage TRB is used to generate the Status stage transaction of a USB
Control transfer. Refer to section 3.2.9 for more information on Control transfers
and the operation of control endpoints.
*/
typedef struct XhciStatusStageTransferRequestBlock {
    // Reserved
    uint64_t rsvd0;

    struct {
        // Reserved
        uint32_t rsvd1              : 22;

        /*
            This field defines the index of the Interrupter that will receive events
            generated by this TRB. Valid values are between 0 and MaxIntrs-1.
        */
        uint32_t interrupterTarget  : 10;
    };

    struct {
        // This bit is used to mark the Enqueue point of a Transfer ring
        uint32_t cycleBit       : 1;

        /*
            Evaluate Next TRB (ENT). If this flag is ‘1’ the xHC shall fetch and evaluate the
            next TRB before saving the endpoint state. Refer to section 4.12.3 for more information.
        */
        uint32_t ent            : 1;

        // Reserved
        uint32_t rsvd2          : 2;

        /*
            Chain bit (CH). Set to ‘1’ by software to associate this TRB with the next TRB on the Ring.
            A Data Stage TD is defined as a Data Stage TRB followed by zero or more Normal TRBs.
            The Chain bit is used to identify a multi-TRB Data Stage TD.
            
            The Chain bit is always ‘0’ in the last TRB of a Data Stage TD.
        */
        uint32_t chain          : 1;

        /*
            Interrupt On Completion (IOC). If this bit is set to ‘1’, it specifies that when this TRB
            completes, the Host Controller shall notify the system of the completion by placing an
            Event TRB on the Event ring and sending an interrupt at the next interrupt threshold.
            Refer to section 4.10.4.
        */
        uint32_t ioc            : 1;

        // Reserved
        uint32_t rsvd3          : 4;

        /*
            TRB Type. This field is set to Setup Stage TRB type.
            Refer to Table 6-91 for the definition of the Type TRB IDs.
        */
        uint32_t trbType        : 6;

        /*
            Direction (DIR). This bit indicates the direction of the data transfer as defined in the
            Data State TRB Direction column of Table 7. If cleared to ‘0’, the data stage transfer
            direction is OUT (Write Data).
            
            If set to ‘1’, the data stage transfer direction is IN (Read Data).
            Refer to section 4.11.2.2 for more information on the use of DIR.
        */
        uint32_t dir            : 1;

        // Reserved
        uint32_t rsvd4          : 15;
    };
} XhciStatusStageTrb_t;
static_assert(sizeof(XhciStatusStageTrb_t) == sizeof(uint32_t) * 4);

/*
// xHci Spec Section 6.4.4.2 Event Data TRB Figure 6-39: Event Data TRB (page 505).

An Event Data TRB allows system software to generate a software defined event
and specify the Parameter field of the generated Transfer Event.

Note: When applying Event Data TRBs to control transfer: 1) An Event Data TRB may
be inserted at the end of a Data Stage TD in order to report the accumulated
transfer length of a multi-TRB TD. 2) An Event Data TRB may be inserted at the
end of a Status Stage TD in order to provide Event Data associated with the
control transfer completion.

Refer to section 4.11.5.2 for more information
*/
typedef struct XhciEventDataTransferRequestBlock {
    /*
        Event Data Hi and Lo. This field represents the 64-bit value that shall be copied to
        the TRB Pointer field (Parameter Component) of the Transfer Event TRB.
    */
    uint64_t data;

    struct {
        // Reserved
        uint32_t rsvd0              : 22;

        /*
            This field defines the index of the Interrupter that will receive events
            generated by this TRB. Valid values are between 0 and MaxIntrs-1.
        */
        uint32_t interrupterTarget  : 10;
    };
    
    struct {
        // This bit is used to mark the Enqueue point of a Transfer ring
        uint32_t cycleBit       : 1;

        /*
            Evaluate Next TRB (ENT). If this flag is ‘1’ the xHC shall fetch and evaluate the
            next TRB before saving the endpoint state. Refer to section 4.12.3 for more information.
        */
        uint32_t ent            : 1;

        // Reserved
        uint32_t rsvd1          : 2;

        /*
            Chain bit (CH). Set to ‘1’ by software to associate this TRB with the next TRB on the Ring.
            A Data Stage TD is defined as a Data Stage TRB followed by zero or more Normal TRBs.
            The Chain bit is used to identify a multi-TRB Data Stage TD.
            
            The Chain bit is always ‘0’ in the last TRB of a Data Stage TD.
        */
        uint32_t chain          : 1;

        /*
            Interrupt On Completion (IOC). If this bit is set to ‘1’, it specifies that when this TRB
            completes, the Host Controller shall notify the system of the completion by placing an
            Event TRB on the Event ring and sending an interrupt at the next interrupt threshold.
            Refer to section 4.10.4.
        */
        uint32_t ioc            : 1;

        // Reserved
        uint32_t rsvd2          : 3;

        /*
            Block Event Interrupt (BEI). If this bit is set to '1' and IOC = '1', then the
            Transfer Event generated by IOC shall not assert an interrupt to the host at the
            next interrupt threshold. Refer to section 4.17.5.
        */
        uint32_t bei            : 1;

        /*
            TRB Type. This field is set to Setup Stage TRB type.
            Refer to Table 6-91 for the definition of the Type TRB IDs.
        */
        uint32_t trbType        : 6;

        // Reserved
        uint32_t rsvd3          : 16;
    };
} XhciEventDataTrb_t;
static_assert(sizeof(XhciEventDataTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciNormalRequestBlock {
    uint64_t dataBufferPhysicalBase;
    union {
        struct {
            uint32_t trbTransferLength  : 17;
            uint32_t tdSize             : 5;
            uint32_t interrupterTarget  : 10;
        };
        uint32_t dword1;
    };
    struct {
        // This bit is used to mark the Enqueue point of a Transfer ring
        uint32_t cycleBit       : 1;

        /*
            Evaluate Next TRB (ENT). If this flag is ‘1’ the xHC shall fetch and evaluate the
            next TRB before saving the endpoint state. Refer to section 4.12.3 for more information.
        */
        uint32_t ent            : 1;

        /*
            Interrupt-on Short Packet (ISP). If this flag is ‘1’ and a Short Packet is encountered
            for this TRB (i.e., less than the amount specified in TRB Transfer Length), then a
            Transfer Event TRB shall be generated with its Completion Code set to Short Packet.
            The TRB Transfer Length field in the Transfer Event TRB shall reflect the residual
            number of bytes not transferred into the associated data buffer. In either case, when
            a Short Packet is encountered, the TRB shall be retired without error and the xHC shall
            advance to the Status Stage TD.

            Note: if the ISP and IOC flags are both ‘1’ and a Short Packet is detected, then only one
            Transfer Event TRB shall be queued to the Event Ring. Also refer to section 4.10.1.1.
        */
        uint32_t isp            : 1;

        /*
            No Snoop (NS). When set to ‘1’, the xHC is permitted to set the No Snoop bit in the
            Requester Attributes of the PCIe transactions it initiates if the PCIe configuration
            Enable No Snoop flag is also set.
            
            When cleared to ‘0’, the xHC is not permitted to set PCIe packet No Snoop Requester
            Attribute. Refer to section 4.18.1 for more information.

            NOTE: If software sets this bit, then it is responsible for maintaining cache consistency.
        */
        uint32_t noSnoop        : 1;

        /*
            Chain bit (CH). Set to ‘1’ by software to associate this TRB with the next TRB on the Ring.
            A Data Stage TD is defined as a Data Stage TRB followed by zero or more Normal TRBs.
            The Chain bit is used to identify a multi-TRB Data Stage TD.
            
            The Chain bit is always ‘0’ in the last TRB of a Data Stage TD.
        */
        uint32_t chain          : 1;

        /*
            Interrupt On Completion (IOC). If this bit is set to ‘1’, it specifies that when this TRB
            completes, the Host Controller shall notify the system of the completion by placing an
            Event TRB on the Event ring and sending an interrupt at the next interrupt threshold.
            Refer to section 4.10.4.
        */
        uint32_t ioc             : 1;

        /*
            Immediate Data (IDT). This bit shall be set to ‘1’ in a Setup Stage TRB.
            It specifies that the Parameter component of this TRB contains Setup Data.
        */
        uint32_t idt            : 1;

        // Reserved
        uint32_t rsvd0          : 2;

        /*
            Block Event Interrupt (BEI). If this bit is set to '1' and IOC = '1', then the
            Transfer Event generated by IOC shall not assert an interrupt to the host at the
            next interrupt threshold. Refer to section 4.17.5.
        */
        uint32_t bei            : 1;

        /*
            TRB Type. This field is set to Setup Stage TRB type.
            Refer to Table 6-91 for the definition of the Type TRB IDs.
        */
        uint32_t trbType        : 6;

        /*
            Direction (DIR). This bit indicates the direction of the data transfer as defined in the
            Data State TRB Direction column of Table 7. If cleared to ‘0’, the data stage transfer
            direction is OUT (Write Data).
            
            If set to ‘1’, the data stage transfer direction is IN (Read Data).
            Refer to section 4.11.2.2 for more information on the use of DIR.
        */
        uint32_t dir            : 1;

        // Reserved
        uint32_t rsvd1          : 15;
    };
} XhciNormalTrb_t;
static_assert(sizeof(XhciNormalTrb_t) == sizeof(uint32_t) * 4);

static inline const char* trbCompletionCodeToString(uint8_t completionCode) {
    switch (completionCode) {
    case XHCI_TRB_COMPLETION_CODE_INVALID:
        return "INVALID";
    case XHCI_TRB_COMPLETION_CODE_SUCCESS:
        return "SUCCESS";
    case XHCI_TRB_COMPLETION_CODE_DATA_BUFFER_ERROR:
        return "DATA_BUFFER_ERROR";
    case XHCI_TRB_COMPLETION_CODE_BABBLE_DETECTED_ERROR:
        return "BABBLE_DETECTED_ERROR";
    case XHCI_TRB_COMPLETION_CODE_USB_TRANSACTION_ERROR:
        return "USB_TRANSACTION_ERROR";
    case XHCI_TRB_COMPLETION_CODE_TRB_ERROR:
        return "TRB_ERROR";
    case XHCI_TRB_COMPLETION_CODE_STALL_ERROR:
        return "STALL_ERROR";
    case XHCI_TRB_COMPLETION_CODE_RESOURCE_ERROR:
        return "RESOURCE_ERROR";
    case XHCI_TRB_COMPLETION_CODE_BANDWIDTH_ERROR:
        return "BANDWIDTH_ERROR";
    case XHCI_TRB_COMPLETION_CODE_NO_SLOTS_AVAILABLE:
        return "NO_SLOTS_AVAILABLE";
    case XHCI_TRB_COMPLETION_CODE_INVALID_STREAM_TYPE:
        return "INVALID_STREAM_TYPE";
    case XHCI_TRB_COMPLETION_CODE_SLOT_NOT_ENABLED:
        return "SLOT_NOT_ENABLED";
    case XHCI_TRB_COMPLETION_CODE_ENDPOINT_NOT_ENABLED:
        return "ENDPOINT_NOT_ENABLED";
    case XHCI_TRB_COMPLETION_CODE_SHORT_PACKET:
        return "SHORT_PACKET";
    case XHCI_TRB_COMPLETION_CODE_RING_UNDERRUN:
        return "RING_UNDERRUN";
    case XHCI_TRB_COMPLETION_CODE_RING_OVERRUN:
        return "RING_OVERRUN";
    case XHCI_TRB_COMPLETION_CODE_VF_EVENT_RING_FULL:
        return "VF_EVENT_RING_FULL";
    case XHCI_TRB_COMPLETION_CODE_PARAMETER_ERROR:
        return "PARAMETER_ERROR";
    case XHCI_TRB_COMPLETION_CODE_BANDWIDTH_OVERRUN:
        return "BANDWIDTH_OVERRUN";
    case XHCI_TRB_COMPLETION_CODE_CONTEXT_STATE_ERROR:
        return "CONTEXT_STATE_ERROR";
    case XHCI_TRB_COMPLETION_CODE_NO_PING_RESPONSE:
        return "NO_PING_RESPONSE";
    case XHCI_TRB_COMPLETION_CODE_EVENT_RING_FULL:
        return "EVENT_RING_FULL";
    case XHCI_TRB_COMPLETION_CODE_INCOMPATIBLE_DEVICE:
        return "INCOMPATIBLE_DEVICE";
    case XHCI_TRB_COMPLETION_CODE_MISSED_SERVICE:
        return "MISSED_SERVICE";
    case XHCI_TRB_COMPLETION_CODE_COMMAND_RING_STOPPED:
        return "COMMAND_RING_STOPPED";
    case XHCI_TRB_COMPLETION_CODE_COMMAND_ABORTED:
        return "COMMAND_ABORTED";
    case XHCI_TRB_COMPLETION_CODE_STOPPED:
        return "STOPPED";
    case XHCI_TRB_COMPLETION_CODE_STOPPED_LENGTH_INVALID:
        return "STOPPED_LENGTH_INVALID";
    case XHCI_TRB_COMPLETION_CODE_STOPPED_SHORT_PACKET:
        return "STOPPED_SHORT_PACKET";
    case XHCI_TRB_COMPLETION_CODE_MAX_EXIT_LATENCY_ERROR:
        return "MAX_EXIT_LATENCY_ERROR";
    default:
        return "UNKNOWN_COMPLETION_CODE";
    }
}

static inline const char* trbTypeToString(uint8_t trbType) {
    switch (trbType) {
    case XHCI_TRB_TYPE_RESERVED: return "XHCI_TRB_TYPE_RESERVED";
    case XHCI_TRB_TYPE_NORMAL: return "XHCI_TRB_TYPE_NORMAL";
    case XHCI_TRB_TYPE_SETUP_STAGE: return "XHCI_TRB_TYPE_SETUP_STAGE";
    case XHCI_TRB_TYPE_DATA_STAGE: return "XHCI_TRB_TYPE_DATA_STAGE";
    case XHCI_TRB_TYPE_STATUS_STAGE: return "XHCI_TRB_TYPE_STATUS_STAGE";
    case XHCI_TRB_TYPE_ISOCH: return "XHCI_TRB_TYPE_ISOCH";
    case XHCI_TRB_TYPE_LINK: return "XHCI_TRB_TYPE_LINK";
    case XHCI_TRB_TYPE_EVENT_DATA: return "XHCI_TRB_TYPE_EVENT_DATA";
    case XHCI_TRB_TYPE_NOOP: return "XHCI_TRB_TYPE_NOOP";
    case XHCI_TRB_TYPE_ENABLE_SLOT_CMD: return "XHCI_TRB_TYPE_ENABLE_SLOT_CMD";
    case XHCI_TRB_TYPE_DISABLE_SLOT_CMD: return "XHCI_TRB_TYPE_DISABLE_SLOT_CMD";
    case XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD: return "XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD";
    case XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD: return "XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD";
    case XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD: return "XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD";
    case XHCI_TRB_TYPE_RESET_ENDPOINT_CMD: return "XHCI_TRB_TYPE_RESET_ENDPOINT_CMD";
    case XHCI_TRB_TYPE_STOP_ENDPOINT_CMD: return "XHCI_TRB_TYPE_STOP_ENDPOINT_CMD";
    case XHCI_TRB_TYPE_SET_TR_DEQUEUE_PTR_CMD: return "XHCI_TRB_TYPE_SET_TR_DEQUEUE_PTR_CMD";
    case XHCI_TRB_TYPE_RESET_DEVICE_CMD: return "XHCI_TRB_TYPE_RESET_DEVICE_CMD";
    case XHCI_TRB_TYPE_FORCE_EVENT_CMD: return "XHCI_TRB_TYPE_FORCE_EVENT_CMD";
    case XHCI_TRB_TYPE_NEGOTIATE_BANDWIDTH_CMD: return "XHCI_TRB_TYPE_NEGOTIATE_BANDWIDTH_CMD";
    case XHCI_TRB_TYPE_SET_LATENCY_TOLERANCE_VALUE_CMD: return "XHCI_TRB_TYPE_SET_LATENCY_TOLERANCE_VALUE_CMD";
    case XHCI_TRB_TYPE_GET_PORT_BANDWIDTH_CMD: return "XHCI_TRB_TYPE_GET_PORT_BANDWIDTH_CMD";
    case XHCI_TRB_TYPE_FORCE_HEADER_CMD: return "XHCI_TRB_TYPE_FORCE_HEADER_CMD";
    case XHCI_TRB_TYPE_NOOP_CMD: return "XHCI_TRB_TYPE_NOOP_CMD";
    case XHCI_TRB_TYPE_GET_EXTENDED_PROPERTY_CMD: return "XHCI_TRB_TYPE_GET_EXTENDED_PROPERTY_CMD";
    case XHCI_TRB_TYPE_SET_EXTENDED_PROPERTY_CMD: return "XHCI_TRB_TYPE_SET_EXTENDED_PROPERTY_CMD";
    case XHCI_TRB_TYPE_TRANSFER_EVENT: return "XHCI_TRB_TYPE_TRANSFER_EVENT";
    case XHCI_TRB_TYPE_CMD_COMPLETION_EVENT: return "XHCI_TRB_TYPE_CMD_COMPLETION_EVENT";
    case XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT: return "XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT";
    case XHCI_TRB_TYPE_BANDWIDTH_REQUEST_EVENT: return "XHCI_TRB_TYPE_BANDWIDTH_REQUEST_EVENT";
    case XHCI_TRB_TYPE_DOORBELL_EVENT: return "XHCI_TRB_TYPE_DOORBELL_EVENT";
    case XHCI_TRB_TYPE_HOST_CONTROLLER_EVENT: return "XHCI_TRB_TYPE_HOST_CONTROLLER_EVENT";
    case XHCI_TRB_TYPE_DEVICE_NOTIFICATION_EVENT: return "XHCI_TRB_TYPE_DEVICE_NOTIFICATION_EVENT";
    default: return "UNKNOWN_TRB_TYPE";
    }
}

#endif
