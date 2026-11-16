#ifndef STELLUX_TESTS_USB_FAKE_XHCI_H
#define STELLUX_TESTS_USB_FAKE_XHCI_H

#include "helpers.h"
#include "stlx_unit_test.h"
#include "drivers/usb/core/usb_device.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/xhci/xhci.h"
#include "drivers/usb/xhci/xhci_common.h"
#include "mm/heap.h"

namespace drivers {

// Puts the fake's rings, registers and device in the driver, and runs the HCD task's steps
struct xhci_hcd_test_access {
    static void install(
        xhci_hcd& hcd,
        xhci::xhci_command_ring* command_ring,
        xhci::xhci_event_ring* event_ring,
        volatile uint32_t* doorbells,
        volatile xhci::xhci_operational_registers* operational,
        xhci::xhci_device* device
    ) {
        hcd.m_cmd_ring = command_ring;
        hcd.m_event_ring = event_ring;
        hcd.m_doorbells = doorbells;
        hcd.m_xhc_op_regs = operational;
        hcd.m_hc_params.max_device_slots = device->slot_id();
        hcd.m_slot_devices[device->slot_id()] = device;
    }

    static void process_events(xhci_hcd& hcd) {
        hcd._process_event_ring();
        hcd.m_event_ring->finish_processing();
    }

    static void stop_cancelled_transfers(xhci_hcd& hcd) {
        hcd._stop_cancelled_transfers();
    }

    static bool is_event_pending(xhci_hcd& hcd) {
        return hcd.m_event_pending;
    }
};

} // namespace drivers

namespace usb_test {

using drivers::xhci_hcd;
using drivers::xhci_hcd_test_access;
using drivers::xhci::xhci_command_completion_trb_t;
using drivers::xhci::xhci_command_ring;
using drivers::xhci::xhci_device;
using drivers::xhci::xhci_endpoint;
using drivers::xhci::xhci_event_ring;
using drivers::xhci::xhci_interrupter_registers;
using drivers::xhci::xhci_operational_registers;
using drivers::xhci::xhci_transfer_completion_trb_t;
using drivers::xhci::xhci_trb_t;

// An `xhci_hcd` with no PCI device behind it, its rings and registers in memory and a full-speed device in slot 1 with
// a bulk IN endpoint. The test plays the controller: it reads what the driver puts on the rings and posts the events
struct fake_xhci {
    static constexpr uint8_t  ROOT_PORT       = 1;
    static constexpr uint8_t  SLOT_ID         = 1;
    static constexpr uint8_t  BULK_IN         = 0x81;
    static constexpr uint8_t  BULK_ATTRIBUTES = 2; // bmAttributes of a bulk endpoint
    static constexpr uint16_t MAX_PACKET      = 64;
    static constexpr uint32_t DOORBELL_COUNT  = 256;
    static constexpr uint32_t CYCLE_BIT       = 1;

    // The HCD task writes each command right after its cue, so one missing after this bound is never written
    static constexpr uint64_t COMMAND_TIMEOUT_NS = 2000000000ULL;

    alignas(xhci_hcd) uint8_t  hcd_bytes[sizeof(xhci_hcd)];
    xhci_hcd*                  hcd;
    volatile uint32_t          doorbells[DOORBELL_COUNT];
    xhci_interrupter_registers interrupter;
    xhci_operational_registers operational;
    xhci_command_ring*         command_ring;
    xhci_event_ring*           event_ring;

    xhci_device*   device;
    xhci_endpoint* endpoint;
    usb::device    usb_device;

    // The event ring entry the next event takes, and the cycle bit it carries
    uint32_t next_event;
    uint8_t  event_cycle;

    // The command ring entries `complete_command` has completed
    uint32_t commands_completed;
};

// A cancel's stop and dequeue move take the command ring's first two entries
constexpr uint32_t STOP_COMMAND = 0;
constexpr uint32_t MOVE_COMMAND = 1;

// A ring's first lap writes cycle bit 1, which a dequeue pointer moved within that lap must carry
constexpr uint64_t FIRST_LAP_CYCLE = 1;

// Builds the driver, its rings and its device in `fake`, which must be zeroed. False when memory runs out
inline bool attach_fake_xhci(fake_xhci& fake) {
    fake.hcd = new (fake.hcd_bytes) xhci_hcd(nullptr);
    fake.command_ring = heap::ualloc_new<xhci_command_ring>();
    fake.event_ring = heap::ualloc_new<xhci_event_ring>();
    fake.device = heap::ualloc_new<xhci_device>();
    fake.endpoint = heap::ualloc_new<xhci_endpoint>();
    if (!fake.command_ring || !fake.event_ring || !fake.device || !fake.endpoint) {
        return false;
    }

    usb::usb_endpoint_descriptor bulk_in = {};
    bulk_in.header = {sizeof(bulk_in), usb::USB_DESCRIPTOR_ENDPOINT};
    bulk_in.bEndpointAddress = fake_xhci::BULK_IN;
    bulk_in.bmAttributes = fake_xhci::BULK_ATTRIBUTES;
    bulk_in.wMaxPacketSize = fake_xhci::MAX_PACKET;

    if (fake.command_ring->init(XHCI_COMMAND_RING_TRB_COUNT) != 0 ||
        fake.event_ring->init(XHCI_EVENT_RING_TRB_COUNT, &fake.interrupter) != 0 ||
        fake.device->init(fake_xhci::ROOT_PORT, fake_xhci::SLOT_ID, XHCI_USB_SPEED_FULL_SPEED, false) != 0 ||
        fake.endpoint->init(fake_xhci::SLOT_ID, &bulk_in) != 0) {
        return false;
    }

    fake.device->set_endpoint(fake.endpoint->dci(), fake.endpoint);
    xhci_hcd_test_access::install(
        *fake.hcd,
        fake.command_ring,
        fake.event_ring,
        fake.doorbells,
        &fake.operational,
        fake.device
    );

    fake.usb_device.hcd = fake.hcd;
    fake.usb_device.hcd_device = fake.device;
    fake.event_cycle = 1;
    return true;
}

// Frees whatever `attach_fake_xhci` built, including a partial build that ran out of memory
inline void detach_fake_xhci(fake_xhci& fake) {
    if (fake.endpoint) {
        if (fake.device) {
            fake.device->set_endpoint(fake.endpoint->dci(), nullptr);
        }

        fake.endpoint->destroy();
        heap::ufree_delete(fake.endpoint);
    }

    if (fake.device) {
        fake.device->destroy();
        heap::ufree_delete(fake.device);
    }

    if (fake.event_ring) {
        fake.event_ring->destroy();
        heap::ufree_delete(fake.event_ring);
    }

    if (fake.command_ring) {
        fake.command_ring->destroy();
        heap::ufree_delete(fake.command_ring);
    }

    if (fake.hcd) {
        fake.hcd->~xhci_hcd();
    }
}

// Gives the device one more endpoint, which the device frees with itself. Null when memory runs out
inline xhci_endpoint* add_fake_endpoint(fake_xhci& fake, uint8_t address, uint8_t attributes, uint16_t max_packet) {
    usb::usb_endpoint_descriptor descriptor = {};
    descriptor.header = {sizeof(descriptor), usb::USB_DESCRIPTOR_ENDPOINT};
    descriptor.bEndpointAddress = address;
    descriptor.bmAttributes = attributes;
    descriptor.wMaxPacketSize = max_packet;

    auto* endpoint = heap::ualloc_new<xhci_endpoint>();
    if (!endpoint) {
        return nullptr;
    }

    if (endpoint->init(fake_xhci::SLOT_ID, &descriptor) != 0) {
        endpoint->destroy();
        heap::ufree_delete(endpoint);
        return nullptr;
    }

    fake.device->set_endpoint(endpoint->dci(), endpoint);
    return endpoint;
}

// The physical address of transfer ring entry `index` of `endpoint`, the bulk IN endpoint unless given
inline uint64_t transfer_trb_phys(const fake_xhci& fake, uint32_t index, xhci_endpoint* endpoint = nullptr) {
    xhci_endpoint* ring_owner = endpoint ? endpoint : fake.endpoint;
    return ring_owner->ring()->get_physical_base() + index * sizeof(xhci_trb_t);
}

// Posts `event` to the event ring as the controller does, with its cycle bit written last
inline void post_event(fake_xhci& fake, const xhci_trb_t& event) {
    xhci_trb_t* slot = &fake.event_ring->get_virtual_base()[fake.next_event];
    slot->parameter = event.parameter;
    slot->status = event.status;
    uint32_t control = (event.control & ~fake_xhci::CYCLE_BIT) | fake.event_cycle;
    __atomic_store_n(&slot->control, control, __ATOMIC_RELEASE);

    fake.next_event++;
    if (fake.next_event == XHCI_EVENT_RING_TRB_COUNT) {
        fake.next_event = 0;
        fake.event_cycle ^= 1;
    }
}

// Posts the completion of transfer ring entry `trb_index` of `endpoint`, the bulk IN endpoint unless given
inline void post_transfer_event(
    fake_xhci& fake,
    uint32_t trb_index,
    uint32_t residual,
    uint8_t completion_code,
    xhci_endpoint* endpoint = nullptr
) {
    xhci_endpoint* ring_owner = endpoint ? endpoint : fake.endpoint;
    xhci_transfer_completion_trb_t event = {};
    event.transfer_trb_pointer = transfer_trb_phys(fake, trb_index, ring_owner);
    event.transfer_length = residual;
    event.completion_code = completion_code;

    event.trb_type = XHCI_TRB_TYPE_TRANSFER_EVENT;
    event.endpoint_id = ring_owner->dci();
    event.slot_id = fake_xhci::SLOT_ID;

    post_event(fake, *reinterpret_cast<const xhci_trb_t*>(&event));
}

// Posts the completion of command ring entry `command_index` for the device in slot 1
inline void post_command_completion(fake_xhci& fake, uint32_t command_index, uint8_t completion_code) {
    xhci_command_completion_trb_t event = {};
    event.command_trb_pointer = fake.command_ring->get_physical_base() + command_index * sizeof(xhci_trb_t);
    event.completion_code = completion_code;
    event.trb_type = XHCI_TRB_TYPE_CMD_COMPLETION_EVENT;
    event.slot_id = fake_xhci::SLOT_ID;
    post_event(fake, *reinterpret_cast<const xhci_trb_t*>(&event));
}

// Wakes the HCD task's wait for an event, as the controller's interrupt does
inline void raise_interrupt(fake_xhci& fake) {
    RUN_ELEVATED(fake.hcd->notify_event());
}

// The command the driver put in command ring entry `index` on the ring's first lap, or null when none arrives in time
inline const xhci_trb_t* wait_for_command(const fake_xhci& fake, uint32_t index) {
    const volatile xhci_trb_t* entry = &fake.command_ring->get_virtual_base()[index];
    uint64_t deadline = clock::now_ns() + fake_xhci::COMMAND_TIMEOUT_NS;
    while ((entry->control & fake_xhci::CYCLE_BIT) == 0) {
        if (clock::now_ns() > deadline) {
            return nullptr;
        }
    }

    return const_cast<const xhci_trb_t*>(entry);
}

// Completes command ring entry `index` with `completion_code` once the driver has written it
inline void complete_command(fake_xhci& fake, uint32_t index, uint8_t completion_code) {
    ASSERT_NOT_NULL(wait_for_command(fake, index));
    post_command_completion(fake, index, completion_code);
    fake.commands_completed = index + 1;
    raise_interrupt(fake);
}

// Answers the Set TR Dequeue Pointer command for `endpoint`, the bulk IN endpoint unless given, whose new dequeue
// pointer must be ring entry `next_entry` with the cycle bit `cycle`
inline void answer_move(
    fake_xhci& fake,
    uint32_t next_entry = 1,
    uint64_t cycle = FIRST_LAP_CYCLE,
    xhci_endpoint* endpoint = nullptr
) {
    xhci_endpoint* moved = endpoint ? endpoint : fake.endpoint;
    auto* move = reinterpret_cast<const drivers::xhci::xhci_set_tr_dequeue_ptr_command_trb_t*>(
        wait_for_command(fake, MOVE_COMMAND)
    );
    ASSERT_NOT_NULL(move);

    EXPECT_EQ(move->trb_type, static_cast<uint32_t>(XHCI_TRB_TYPE_SET_TR_DEQUEUE_PTR_CMD));
    EXPECT_EQ(move->slot_id, fake_xhci::SLOT_ID);
    EXPECT_EQ(move->endpoint_id, moved->dci());
    EXPECT_EQ(move->new_dequeue_ptr, transfer_trb_phys(fake, next_entry, moved) | cycle);
    complete_command(fake, MOVE_COMMAND, XHCI_TRB_COMPLETION_CODE_SUCCESS);
}

inline int32_t code_of(usb::transfer_status status) {
    return static_cast<int32_t>(status);
}

// Transfer ring entry `index` of `endpoint`, the bulk IN endpoint unless given, on the ring's first lap, or null when
// the driver has not written it
inline const xhci_trb_t* transfer_trb(const fake_xhci& fake, uint32_t index, xhci_endpoint* endpoint = nullptr) {
    xhci_endpoint* ring_owner = endpoint ? endpoint : fake.endpoint;
    const xhci_trb_t* entry = &ring_owner->ring()->get_virtual_base()[index];
    return (entry->control & fake_xhci::CYCLE_BIT) ? entry : nullptr;
}

} // namespace usb_test

#endif // STELLUX_TESTS_USB_FAKE_XHCI_H
