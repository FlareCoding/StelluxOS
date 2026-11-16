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

// Gives the fake controller access to the private members of `xhci_hcd`, which declares it a friend
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

    // Gives the driver the device context base address array and the table of the device on each root port
    static void install_device_tables(xhci_hcd& hcd, uint64_t* dcbaa, xhci::xhci_device** port_devices, uint8_t ports) {
        hcd.m_dcbaa = dcbaa;
        hcd.m_port_devices = port_devices;
        hcd.m_hc_params.max_ports = ports;
    }

    static xhci::xhci_device* slot_device(xhci_hcd& hcd, uint8_t slot_id) {
        return hcd.m_slot_devices[slot_id];
    }

    static void record_slot_device(xhci_hcd& hcd, xhci::xhci_device* device) {
        hcd.m_slot_devices[device->slot_id()] = device;
    }

    static void forget_slot_device(xhci_hcd& hcd, xhci::xhci_device* device) {
        hcd.m_slot_devices[device->slot_id()] = nullptr;
    }

    static xhci::xhci_device* port_device(xhci_hcd& hcd, uint8_t port_index) {
        return hcd.m_port_devices[port_index];
    }

    static void record_port_device(xhci_hcd& hcd, uint8_t port_index, xhci::xhci_device* device) {
        hcd.m_port_devices[port_index] = device;
    }

    static void process_events(xhci_hcd& hcd) {
        hcd._process_event_ring();
    }

    static void stop_requested_endpoints(xhci_hcd& hcd) {
        hcd._stop_requested_endpoints();
    }

    static bool is_event_pending(xhci_hcd& hcd) {
        return hcd.m_event_pending;
    }

    static void set_task(xhci_hcd& hcd, sched::task* task) {
        hcd.m_task = task;
    }

    static int32_t send_command(xhci_hcd& hcd, xhci::xhci_trb_t* command) {
        return hcd._send_command(command);
    }

    static int32_t reset_port(xhci_hcd& hcd, uint8_t port_index) {
        return hcd._reset_port(port_index);
    }

    static void wait_us_handling_events(xhci_hcd& hcd, uint64_t duration_us) {
        hcd._wait_us_handling_events(duration_us);
    }

    static void setup_hub_device(xhci_hcd& hcd, xhci::xhci_device* hub, uint8_t hub_port, uint8_t speed) {
        hcd._setup_hub_device(hub, hub_port, speed);
    }

    static void setup_device(xhci_hcd& hcd, uint8_t port_index) {
        hcd._setup_device(port_index);
    }

    // Records a change on root port `port_index` as a pass over the event ring does, then handles it as the main loop
    // does
    static void process_port_change(xhci_hcd& hcd, uint8_t port_index) {
        hcd.m_psc_pending_bitmap |= 1u << port_index;
        hcd._process_pending_port_changes();
    }

    // Takes the oldest hub event and counts it handled, as the main loop does once it has acted on one. False when none
    // is queued
    static bool take_hub_event(xhci_hcd& hcd, xhci::hub_event* event_out) {
        bool has_event = false;
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(hcd.m_hub_event_lock);
            if (hcd.m_hub_event_head != hcd.m_hub_event_tail) {
                *event_out = hcd.m_hub_events[hcd.m_hub_event_head];
                hcd.m_hub_event_head = (hcd.m_hub_event_head + 1) % xhci::HUB_EVENT_QUEUE_SIZE;
                has_event = true;
            }

            sync::spin_unlock_irqrestore(hcd.m_hub_event_lock, irq);
        });

        if (has_event) {
            hcd._complete_hub_event();
        }

        return has_event;
    }

    // The teardown's steps before and after the drivers' `disconnect()`: marking the device's endpoints as
    // disconnecting, then ending their transfers and streams
    static void mark_unplugged(xhci_hcd& hcd, xhci::xhci_device* device) {
        hcd._mark_async_endpoints_for_device_disconnecting(device);
    }

    static void end_unplugged_transfers(xhci_hcd& hcd, xhci::xhci_device* device) {
        hcd._clear_async_endpoints_for_device(device, usb::transfer_status::device_gone);
    }
};

} // namespace drivers

namespace usb_test {

using drivers::xhci_hcd;
using drivers::xhci_hcd_test_access;
using drivers::xhci::xhci_command_completion_trb_t;
using drivers::xhci::xhci_command_ring;
using drivers::xhci::xhci_device;
using drivers::xhci::xhci_device_request_packet;
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

    // The root ports, whose register sets follow the operational registers
    static constexpr uint8_t PORT_COUNT        = 4;
    static constexpr size_t  OPERATIONAL_BYTES = drivers::xhci::PORT_REGS_BASE_OFFSET +
                                                 PORT_COUNT * drivers::xhci::PORT_REGS_STRIDE;

    // The HCD task writes each command right after its cue, so one missing after this bound is never written
    static constexpr uint64_t COMMAND_TIMEOUT_NS = 2000000000ULL;

    // The HCD task rings a control transfer's doorbell right after queuing it, so one missing after this bound is
    // never rung
    static constexpr uint64_t TRANSFER_TIMEOUT_NS = 2000000000ULL;

    // A port reset starts at most the power-on wait after its cue, so one missing after this bound never starts
    static constexpr uint64_t PORT_RESET_TIMEOUT_NS = 2000000000ULL;

    // The device context base address array's entries: the scratchpad array's address, then a context per slot ID
    static constexpr uint32_t DCBAA_ENTRIES = 256;

    alignas(xhci_hcd) uint8_t  hcd_bytes[sizeof(xhci_hcd)];
    xhci_hcd*                  hcd;
    volatile uint32_t          doorbells[DOORBELL_COUNT];
    xhci_interrupter_registers interrupter;

    union {
        xhci_operational_registers operational;
        uint8_t                    operational_space[OPERATIONAL_BYTES];
    };

    xhci_command_ring*         command_ring;
    xhci_event_ring*           event_ring;

    xhci_device*   device;
    xhci_endpoint* endpoint;
    usb::device    usb_device;

    // The device context base address array and each root port's device, which the driver records
    uint64_t     dcbaa[DCBAA_ENTRIES];
    xhci_device* port_devices[PORT_COUNT];

    // The event ring entry the next event takes, and the cycle bit it carries
    uint32_t next_event;
    uint8_t  event_cycle;

    // The command ring entries `complete_command` has completed
    uint32_t commands_completed;

    // A step of the driver that `start_step` runs on a task of its own, what it returned and whether it has ended
    void                   (*step)(fake_xhci& fake);
    int32_t                step_result;
    sched::task*           step_task;
    sync::atomic<uint32_t> is_step_done;

    // The CPU time the step's task had used and the clock when the step returned
    uint64_t step_cpu_ns;
    uint64_t step_end_ns;

    // Whether the device the fake controller plays on a new connection answers its first read of each descriptor
    // implausibly, and the reads it has answered so
    bool    should_answer_first_reads_implausibly;
    uint8_t implausibly_answered_reads;

    // The root ports whose acknowledgment of every change the fake controller has logged, a bit per port
    uint8_t acknowledged_ports;
};

// The kinds of request the fake controller serves while it plays a newly connected device, and the driver's
// acknowledgment of every change on a root port, which it logs as it sees one
enum class served_kind : uint8_t {
    command,
    port_reset,
    control_transfer,
    port_acknowledgment,
};

// A request the fake controller served, with the step task's CPU time and the clock once the task blocked on it
struct served_request {
    served_kind kind;
    uint8_t     code; // the command's TRB type or the control request's `bRequest`
    uint8_t     id;   // the slot ID the command names or completes with, the port's index, or the transfer's slot ID
    uint64_t    cpu_ns;
    uint64_t    at_ns;
};

constexpr size_t MAX_SERVED_REQUESTS = 32;

// The requests the fake controller served, in the order it served them
struct served_log {
    served_request requests[MAX_SERVED_REQUESTS];
    size_t         count;
};

// A cancel's or a close's stop and dequeue move take the command ring's first two entries
constexpr uint32_t STOP_COMMAND = 0;
constexpr uint32_t MOVE_COMMAND = 1;

// A ring's first lap writes cycle bit 1, which a dequeue pointer moved within that lap must carry
constexpr uint64_t FIRST_LAP_CYCLE = 1;

// The device context index of every device's control endpoint
constexpr uint8_t CONTROL_DCI = 1;

// The device the fake controller plays on a new connection: high speed, no strings, and one configuration with no
// interfaces, so no class driver binds to it
constexpr uint16_t NEW_DEVICE_USB_RELEASE          = 0x0200;
constexpr uint8_t  NEW_DEVICE_CONTROL_PACKET_BYTES = 64;
constexpr uint8_t  NEW_DEVICE_CONFIGURATION        = 1;
constexpr uint8_t  CONFIGURATION_BYTES             = 9;

// The descriptor reads that device can answer implausibly: the device descriptor's first 8 bytes, the whole device
// descriptor and the configuration
constexpr uint8_t DEVICE_DESCRIPTOR_START_READ = 1u << 0;
constexpr uint8_t DEVICE_DESCRIPTOR_READ       = 1u << 1;
constexpr uint8_t CONFIGURATION_READ           = 1u << 2;

// A control packet size that high speed does not allow, which makes the first 8 bytes implausible
constexpr uint8_t IMPLAUSIBLE_CONTROL_PACKET_BYTES = 8;

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
    xhci_hcd_test_access::install_device_tables(*fake.hcd, fake.dcbaa, fake.port_devices, fake_xhci::PORT_COUNT);

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

// A high-speed device on root port 1 in slot `slot_id`, recorded in the driver's slot table for it to tear down.
// Null when memory runs out
inline xhci_device* add_fake_device(fake_xhci& fake, uint8_t slot_id) {
    auto* device = heap::ualloc_new<xhci_device>();
    if (!device) {
        return nullptr;
    }

    if (device->init(fake_xhci::ROOT_PORT, slot_id, XHCI_USB_SPEED_HIGH_SPEED, false) != 0) {
        heap::ufree_delete(device);
        return nullptr;
    }

    xhci_hcd_test_access::record_slot_device(*fake.hcd, device);

    return device;
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

// Posts the completion of command ring entry `command_index` for the device in slot `slot_id`, slot 1 unless given
inline void post_command_completion(
    fake_xhci& fake,
    uint32_t command_index,
    uint8_t completion_code,
    uint8_t slot_id = fake_xhci::SLOT_ID
) {
    xhci_command_completion_trb_t event = {};
    event.command_trb_pointer = fake.command_ring->get_physical_base() + command_index * sizeof(xhci_trb_t);
    event.completion_code = completion_code;
    event.trb_type = XHCI_TRB_TYPE_CMD_COMPLETION_EVENT;
    event.slot_id = slot_id;
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

// The status and control register of root port `port_index`
inline volatile uint32_t* portsc(fake_xhci& fake, uint8_t port_index) {
    uint32_t offset = drivers::xhci::PORT_REGS_BASE_OFFSET + port_index * drivers::xhci::PORT_REGS_STRIDE;
    return reinterpret_cast<volatile uint32_t*>(&fake.operational_space[offset]);
}

// Whether the driver has started a reset of root port `port_index` that the fake controller has not ended
inline bool is_port_resetting(fake_xhci& fake, uint8_t port_index) {
    drivers::xhci::xhci_portsc_register port_status;
    port_status.raw = *portsc(fake, port_index);
    return port_status.pr;
}

// Waits for the driver to start a reset of root port `port_index`. False when none starts within
// `PORT_RESET_TIMEOUT_NS`
inline bool wait_for_port_reset(fake_xhci& fake, uint8_t port_index) {
    uint64_t deadline = clock::now_ns() + fake_xhci::PORT_RESET_TIMEOUT_NS;

    while (!is_port_resetting(fake, port_index)) {
        if (clock::now_ns() > deadline) {
            return false;
        }
    }

    return true;
}

// Ends the reset of root port `port_index` with the port enabled, posts its Port Status Change Event and raises the
// interrupt
inline void complete_port_reset(fake_xhci& fake, uint8_t port_index) {
    drivers::xhci::xhci_portsc_register port_status;
    port_status.raw = *portsc(fake, port_index);
    port_status.pr = 0;
    port_status.ped = 1;
    port_status.prc = 1;
    *portsc(fake, port_index) = port_status.raw;

    drivers::xhci::xhci_port_status_change_trb_t event = {};
    event.port_id = port_index + 1;
    event.completion_code = XHCI_TRB_COMPLETION_CODE_SUCCESS;
    event.trb_type = XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT;

    post_event(fake, *reinterpret_cast<const xhci_trb_t*>(&event));
    raise_interrupt(fake);
}

// Runs the fake's step as the driver's own task and records its CPU time and the clock as it returns, then marks it
// ended and ends the task
inline void run_step(void* arg) {
    auto* fake = static_cast<fake_xhci*>(arg);
    xhci_hcd_test_access::set_task(*fake->hcd, sched::current());
    fake->step(*fake);
    RUN_ELEVATED(fake->step_cpu_ns = sched::read_task_cpu_time_ns(sched::current()));
    fake->step_end_ns = clock::now_ns();

    xhci_hcd_test_access::set_task(*fake->hcd, nullptr);
    fake->is_step_done.store_release(1);
    sched::exit(0);
}

// Runs `step` on a task of its own, lowered as the HCD task is and taken by the driver for its task, while the test
// plays the controller. False when the task cannot be created
inline bool start_step(fake_xhci& fake, void (*step)(fake_xhci& fake)) {
    fake.step = step;
    fake.is_step_done.store_relaxed(0);
    fake.step_task = test_helpers::start_pinned_task_on_another_cpu(run_step, &fake, "usb_hcd_step", 0);
    return fake.step_task != nullptr;
}

// Waits for the step to end, raising the interrupt after `COMMAND_TIMEOUT_NS` so a driver wait past its deadline wakes
// and ends. False after `test_helpers::SPIN_TIMEOUT_NS`, when the task and the fake must stay allocated
inline bool finish_step(fake_xhci& fake) {
    uint64_t start = clock::now_ns();

    while (!fake.is_step_done.load_acquire()) {
        uint64_t waited = clock::now_ns() - start;
        if (waited > test_helpers::SPIN_TIMEOUT_NS) {
            return false;
        }

        if (waited > fake_xhci::COMMAND_TIMEOUT_NS) {
            raise_interrupt(fake);
            test_helpers::brief_delay();
        }
    }

    test_helpers::unpin(fake.step_task);
    fake.step_task = nullptr;

    return true;
}

// Whether the driver rang doorbell `slot_id` with `target`, clearing the doorbell when it did so the next ring shows.
// The driver rings a device's doorbell only with targets above 0
inline bool take_doorbell(fake_xhci& fake, uint8_t slot_id, uint32_t target) {
    if (fake.doorbells[slot_id] != target) {
        return false;
    }

    fake.doorbells[slot_id] = 0;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    return true;
}

// Waits up to `timeout_ns` for `take_doorbell` to find doorbell `slot_id` rung with `target`
inline bool wait_for_doorbell(fake_xhci& fake, uint8_t slot_id, uint32_t target, uint64_t timeout_ns) {
    uint64_t deadline = clock::now_ns() + timeout_ns;

    while (!take_doorbell(fake, slot_id, target)) {
        if (clock::now_ns() > deadline) {
            return false;
        }
    }

    return true;
}

// Gives the request of the last control transfer queued on the control endpoint of `device` in `request_out`. False
// when the ring's first lap holds none
inline bool read_control_request(xhci_device* device, xhci_device_request_packet* request_out) {
    const xhci_trb_t* ring = device->ctrl_ring()->get_virtual_base();
    const xhci_trb_t* setup = nullptr;
    for (size_t i = 0; ring[i].control & fake_xhci::CYCLE_BIT; i++) {
        if (ring[i].trb_type == XHCI_TRB_TYPE_SETUP_STAGE) {
            setup = &ring[i];
        }
    }

    if (!setup) {
        return false;
    }

    *request_out = reinterpret_cast<const drivers::xhci::xhci_setup_stage_trb_t*>(setup)->request_packet;

    return true;
}

// Waits for the driver to ring the control endpoint of `device` and gives the request it queued in `request_out`.
// False when no transfer comes within `TRANSFER_TIMEOUT_NS`
inline bool take_control_transfer(fake_xhci& fake, xhci_device* device, xhci_device_request_packet* request_out) {
    uint32_t control_ring = XHCI_DOORBELL_TARGET_CONTROL_EP_RING;
    if (!wait_for_doorbell(fake, device->slot_id(), control_ring, fake_xhci::TRANSFER_TIMEOUT_NS)) {
        return false;
    }

    return read_control_request(device, request_out);
}

// Completes the last control transfer queued on the control endpoint of `device` with `completion_code`, success
// unless given, giving an IN transfer the first `reply_length` bytes of `reply`, and raises the interrupt
inline void complete_control_transfer(
    fake_xhci& fake,
    xhci_device* device,
    const void* reply,
    uint16_t reply_length,
    uint8_t completion_code = XHCI_TRB_COMPLETION_CODE_SUCCESS
) {
    if (reply_length > 0) {
        string::memcpy(device->ctrl_transfer_buffer(), reply, reply_length);
    }

    xhci_transfer_completion_trb_t event = {};
    event.completion_code = completion_code;
    event.trb_type = XHCI_TRB_TYPE_TRANSFER_EVENT;
    event.endpoint_id = CONTROL_DCI;
    event.slot_id = device->slot_id();

    post_event(fake, *reinterpret_cast<const xhci_trb_t*>(&event));
    raise_interrupt(fake);
}

// Waits for the step's task to block and leave its CPU, when its switch has charged all it ran, then gives its CPU time
// in `cpu_ns_out`. False when the step ends or `COMMAND_TIMEOUT_NS` passes first
inline bool read_blocked_step_cpu_ns(fake_xhci& fake, uint64_t* cpu_ns_out) {
    sched::task* task = fake.step_task;
    uint64_t deadline = clock::now_ns() + fake_xhci::COMMAND_TIMEOUT_NS;

    while (true) {
        bool is_blocked = task->state.load_acquire() == sched::TASK_STATE_BLOCKED;
        bool is_off_cpu = sync::atomic_ref<uint32_t>{task->exec.on_cpu}.load_acquire() == 0;
        if (is_blocked && is_off_cpu) {
            break;
        }

        if (fake.is_step_done.load_acquire() || clock::now_ns() > deadline) {
            return false;
        }
    }

    RUN_ELEVATED(*cpu_ns_out = sched::read_task_cpu_time_ns(task));

    return true;
}

// Appends a request to the log, which keeps the first `MAX_SERVED_REQUESTS`, with the CPU time and the clock once the
// step's task blocks on it. False when the task does not block
inline bool record_served(fake_xhci& fake, served_log& served, served_kind kind, uint8_t code, uint8_t id) {
    uint64_t cpu_ns = 0;
    if (!read_blocked_step_cpu_ns(fake, &cpu_ns)) {
        return false;
    }

    if (served.count < MAX_SERVED_REQUESTS) {
        served.requests[served.count++] = {kind, code, id, cpu_ns, clock::now_ns()};
    }

    return true;
}

// Fills in the device descriptor of the device the fake controller plays on a new connection
inline void fill_new_device_descriptor(usb::usb_device_descriptor& descriptor) {
    descriptor.header.bLength = static_cast<uint8_t>(sizeof(descriptor));
    descriptor.header.bDescriptorType = usb::USB_DESCRIPTOR_DEVICE;
    descriptor.bcdUsb = NEW_DEVICE_USB_RELEASE;
    descriptor.bMaxPacketSize0 = NEW_DEVICE_CONTROL_PACKET_BYTES;
    descriptor.bNumConfigurations = 1;
}

// Fills in that device's configuration descriptor, which has no interfaces
inline void fill_new_device_configuration(usb::usb_configuration_descriptor& configuration) {
    configuration.header.bLength = CONFIGURATION_BYTES;
    configuration.header.bDescriptorType = usb::USB_DESCRIPTOR_CONFIGURATION;
    configuration.wTotalLength = CONFIGURATION_BYTES;
    configuration.bConfigurationValue = NEW_DEVICE_CONFIGURATION;
}

// Makes the answer to `read` implausible: a control packet size high speed does not allow, no configurations, or a
// configuration header of another descriptor's type
inline void spoil_answer(
    uint8_t read,
    usb::usb_device_descriptor& descriptor,
    usb::usb_configuration_descriptor& configuration
) {
    if (read == DEVICE_DESCRIPTOR_START_READ) {
        descriptor.bMaxPacketSize0 = IMPLAUSIBLE_CONTROL_PACKET_BYTES;
    } else if (read == DEVICE_DESCRIPTOR_READ) {
        descriptor.bNumConfigurations = 0;
    } else if (read == CONFIGURATION_READ) {
        configuration.header.bDescriptorType = usb::USB_DESCRIPTOR_DEVICE;
    }
}

// Logs each root port that shows its config error change bit for the first time, which in memory only the driver's
// acknowledgment of every change sets. Only reads, so no write of the fake's undoes one of the driver's
inline void record_port_acknowledgments(fake_xhci& fake, served_log& served) {
    for (uint8_t i = 0; i < fake_xhci::PORT_COUNT; i++) {
        drivers::xhci::xhci_portsc_register port_status;
        port_status.raw = *portsc(fake, i);
        bool is_logged = fake.acknowledged_ports & (1u << i);
        if (!port_status.cec || is_logged || served.count >= MAX_SERVED_REQUESTS) {
            continue;
        }

        fake.acknowledged_ports |= 1u << i;
        served.requests[served.count++] = {served_kind::port_acknowledgment, 0, i, 0, clock::now_ns()};
    }
}

// Completes the next command the driver queued, if it has queued one, handing `*next_slot_id` to an Enable Slot and
// incrementing it. False when the driver does not block on the command
inline bool serve_command(fake_xhci& fake, uint8_t* next_slot_id, served_log& served) {
    const volatile xhci_trb_t* entry = &fake.command_ring->get_virtual_base()[fake.commands_completed];
    uint32_t control = entry->control;
    if ((control & fake_xhci::CYCLE_BIT) == 0) {
        return true;
    }

    // A port acknowledged before this command shows it by now, so the log keeps the driver's order
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    record_port_acknowledgments(fake, served);

    // Of the commands setup and teardown send, all but Enable Slot name their slot ID in the control word's top byte
    uint8_t type = static_cast<uint8_t>((control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT);
    uint8_t slot_id = type == XHCI_TRB_TYPE_ENABLE_SLOT_CMD ? (*next_slot_id)++ : static_cast<uint8_t>(control >> 24);
    if (!record_served(fake, served, served_kind::command, type, slot_id)) {
        return false;
    }

    post_command_completion(fake, fake.commands_completed, XHCI_TRB_COMPLETION_CODE_SUCCESS, slot_id);
    fake.commands_completed++;
    raise_interrupt(fake);

    return true;
}

// Completes the control transfer queued on the control endpoint of `device`, answering GET_DESCRIPTOR for the new
// device and its configuration and moving no data for any other request. False when the driver does not block on it
inline bool serve_control_transfer(fake_xhci& fake, xhci_device* device, served_log& served) {
    xhci_device_request_packet request = {};
    if (!read_control_request(device, &request)) {
        return true;
    }

    usb::usb_device_descriptor descriptor = {};
    usb::usb_configuration_descriptor configuration = {};
    fill_new_device_descriptor(descriptor);
    fill_new_device_configuration(configuration);

    bool is_get_descriptor = request.bRequest == usb::USB_REQUEST_GET_DESCRIPTOR;
    uint8_t descriptor_type = static_cast<uint8_t>(request.wValue >> 8);
    const void* reply = nullptr;
    uint16_t reply_bytes = 0;
    uint8_t read = 0;

    if (is_get_descriptor && descriptor_type == usb::USB_DESCRIPTOR_DEVICE) {
        reply = &descriptor;
        reply_bytes = sizeof(descriptor);
        read = request.wLength < sizeof(descriptor) ? DEVICE_DESCRIPTOR_START_READ : DEVICE_DESCRIPTOR_READ;
    } else if (is_get_descriptor && descriptor_type == usb::USB_DESCRIPTOR_CONFIGURATION) {
        reply = &configuration;
        reply_bytes = CONFIGURATION_BYTES;
        read = CONFIGURATION_READ;
    }

    bool is_first_read = read != 0 && (fake.implausibly_answered_reads & read) == 0;
    if (fake.should_answer_first_reads_implausibly && is_first_read) {
        fake.implausibly_answered_reads |= read;
        spoil_answer(read, descriptor, configuration);
    }

    if (!record_served(fake, served, served_kind::control_transfer, request.bRequest, device->slot_id())) {
        return false;
    }

    uint16_t reply_length = request.wLength < reply_bytes ? request.wLength : reply_bytes;
    complete_control_transfer(fake, device, reply, reply_length);

    return true;
}

// Plays the controller and a new device until the step ends, in place of `finish_step`, completing each request with
// Enable Slot IDs from `first_slot_id` up. False when the driver does not block on a request or the step does not end
inline bool serve_until_step_done(fake_xhci& fake, uint8_t first_slot_id, served_log& served) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    uint8_t next_slot_id = first_slot_id;

    while (!fake.is_step_done.load_acquire()) {
        if (clock::now_ns() > deadline || !serve_command(fake, &next_slot_id, served)) {
            return false;
        }

        for (uint8_t i = 0; i < fake_xhci::PORT_COUNT; i++) {
            if (!is_port_resetting(fake, i)) {
                continue;
            }

            if (!record_served(fake, served, served_kind::port_reset, 0, i)) {
                return false;
            }

            complete_port_reset(fake, i);
        }

        for (uint32_t i = 1; i < fake_xhci::DOORBELL_COUNT; i++) {
            uint8_t slot_id = static_cast<uint8_t>(i);
            xhci_device* device = xhci_hcd_test_access::slot_device(*fake.hcd, slot_id);
            bool is_rung = device && take_doorbell(fake, slot_id, XHCI_DOORBELL_TARGET_CONTROL_EP_RING);
            if (is_rung && !serve_control_transfer(fake, device, served)) {
                return false;
            }
        }
    }

    test_helpers::unpin(fake.step_task);
    fake.step_task = nullptr;

    return true;
}

// The physical address of EP0's transfer ring entry `index`
inline uint64_t control_trb_phys(const fake_xhci& fake, uint32_t index) {
    return fake.device->ctrl_ring()->get_physical_base() + index * sizeof(xhci_trb_t);
}

// EP0's transfer ring entry `index` on the ring's first lap, or null when the driver has not written it
inline const xhci_trb_t* control_trb(const fake_xhci& fake, uint32_t index) {
    const xhci_trb_t* entry = &fake.device->ctrl_ring()->get_virtual_base()[index];
    return (entry->control & fake_xhci::CYCLE_BIT) ? entry : nullptr;
}

// EP0's transfer ring entry `index` on the ring's first lap once the driver writes it, or null when it writes none
// within `SPIN_TIMEOUT_NS`, since a class driver's task writes its TD only once it holds EP0
inline const xhci_trb_t* wait_for_control_trb(const fake_xhci& fake, uint32_t index) {
    const volatile xhci_trb_t* entry = &fake.device->ctrl_ring()->get_virtual_base()[index];
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while ((entry->control & fake_xhci::CYCLE_BIT) == 0) {
        if (clock::now_ns() > deadline) {
            return nullptr;
        }
    }

    return const_cast<const xhci_trb_t*>(entry);
}

// Posts the completion of EP0's transfer ring entry `trb_index`, the status stage of a control transfer's TD
inline void post_control_event(fake_xhci& fake, uint32_t trb_index, uint8_t completion_code) {
    xhci_transfer_completion_trb_t event = {};
    event.transfer_trb_pointer = control_trb_phys(fake, trb_index);
    event.completion_code = completion_code;

    event.trb_type = XHCI_TRB_TYPE_TRANSFER_EVENT;
    event.endpoint_id = CONTROL_DCI;
    event.slot_id = fake_xhci::SLOT_ID;

    post_event(fake, *reinterpret_cast<const xhci_trb_t*>(&event));
}

} // namespace usb_test

#endif // STELLUX_TESTS_USB_FAKE_XHCI_H
