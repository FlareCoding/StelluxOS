#ifndef STELLUX_TESTS_SERIAL_FAKE_CHIP_H
#define STELLUX_TESTS_SERIAL_FAKE_CHIP_H

#include "helpers.h"
#include "stlx_unit_test.h"
#include "tests/usb/fake_xhci.h"
#include "drivers/usb/usb_descriptors.h"
#include "clock/clock.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "sync/atomic.h"

namespace serial_test {

using usb_test::fake_xhci;

// The most data bytes a recorded request keeps
constexpr uint16_t MAX_REQUEST_DATA = 16;

// A chip request as the chip receives it
struct chip_request {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    uint8_t  data[MAX_REQUEST_DATA];
};

// The bytes the chip answers one read request with
struct chip_reply {
    const uint8_t* bytes;
    uint16_t       length;
};

// A bridge chip a test plays on the in-memory controller, answering the chip requests of the code under test. Those
// calls run on a lowered task of their own, since each waits for the test's answer
struct fake_chip {
    fake_xhci*             controller;
    uint32_t               next_trb;       // the EP0 ring entry the next request starts at
    uint32_t               next_command;   // the command ring entry the next recovery of EP0 starts at
    void                 (*calls)();
    sync::atomic<uint32_t> are_calls_done;
    sync::atomic<uint32_t> is_recovered;
    bool                   is_call_stuck;  // set once calls outlive their wait, so nothing they reach is freed
};

// Builds the chip's controller on the unprivileged heap. False when memory runs out
inline bool attach_fake_chip(fake_chip& chip) {
    chip.controller = static_cast<fake_xhci*>(heap::uzalloc(sizeof(fake_xhci)));
    if (!chip.controller) {
        return false;
    }

    if (!usb_test::attach_fake_xhci(*chip.controller)) {
        usb_test::detach_fake_xhci(*chip.controller);
        heap::ufree(chip.controller);
        return false;
    }

    chip.next_trb = 0;
    chip.next_command = 0;
    chip.is_call_stuck = false;
    return true;
}

// Frees the controller, unless a call outlived its wait and may still reach it
inline void detach_fake_chip(fake_chip& chip) {
    if (chip.is_call_stuck) {
        return;
    }

    usb_test::detach_fake_xhci(*chip.controller);
    heap::ufree(chip.controller);
}

inline void run_fake_chip_calls(void* arg) {
    auto* chip = static_cast<fake_chip*>(arg);
    chip->calls();
    chip->are_calls_done.store_release(1);
    sched::exit(0);
}

// Records the next request once all of its stages are queued, and gives the EP0 ring entry of its status stage. False
// when no request arrives
inline bool receive_request(fake_chip& chip, chip_request& request, uint32_t* status_trb) {
    using drivers::xhci::xhci_setup_stage_trb_t;
    auto* setup = reinterpret_cast<const xhci_setup_stage_trb_t*>(
        usb_test::wait_for_control_trb(*chip.controller, chip.next_trb)
    );
    if (!setup) {
        return false;
    }

    request.request_type = setup->request_packet.bRequestType;
    request.request = setup->request_packet.bRequest;
    request.value = setup->request_packet.wValue;
    request.index = setup->request_packet.wIndex;
    request.length = setup->request_packet.wLength;

    *status_trb = chip.next_trb + (request.length > 0 ? 2 : 1);
    return usb_test::wait_for_control_trb(*chip.controller, *status_trb) != nullptr;
}

// Plays the chip for the next request: records it, answers a read with `reply`, and completes it. False when no
// request arrives
inline bool answer_request(fake_chip& chip, chip_request& request, const uint8_t* reply, uint16_t reply_length) {
    uint32_t status_trb = 0;
    if (!receive_request(chip, request, &status_trb)) {
        return false;
    }

    auto* buffer = static_cast<uint8_t*>(chip.controller->device->ctrl_transfer_buffer());
    if (request.request_type & usb::USB_REQTYPE_DIR_IN) {
        string::memcpy(buffer, reply, reply_length);
    } else {
        uint16_t kept = request.length < sizeof(request.data) ? request.length : sizeof(request.data);
        string::memcpy(request.data, buffer, kept);
    }

    usb_test::post_control_event(*chip.controller, status_trb, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    drivers::xhci_hcd_test_access::process_events(*chip.controller->hcd);
    chip.next_trb = status_trb + 1;
    return true;
}

// Waits until the stalled request has asked the HCD task to recover EP0. False when the spin times out
inline bool wait_for_recovery_request(fake_chip& chip) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (clock::now_ns() < deadline) {
        bool is_requested = false;
        RUN_ELEVATED({
            drivers::xhci::xhci_device* device = chip.controller->device;
            sync::irq_state irq = sync::spin_lock_irqsave(device->ctrl_completion_lock());
            is_requested = device->ctrl_recovery_requested();
            sync::spin_unlock_irqrestore(device->ctrl_completion_lock(), irq);
        });

        if (is_requested) {
            return true;
        }
    }

    return false;
}

// Runs the HCD task's step that recovers a stalled EP0
inline void run_recovery_step(void* arg) {
    auto* chip = static_cast<fake_chip*>(arg);
    drivers::xhci_hcd_test_access::set_task(*chip->controller->hcd, sched::current());
    drivers::xhci_hcd_test_access::stop_requested_endpoints(*chip->controller->hcd);
    chip->is_recovered.store_release(1);
    sched::exit(0);
}

// Plays the chip refusing the next request with a stall, then the HCD task's recovery of EP0, which the request waits
// for before it fails. False when the request or the recovery's commands never come
inline bool refuse_request(fake_chip& chip, chip_request& request) {
    uint32_t status_trb = 0;
    if (!receive_request(chip, request, &status_trb)) {
        return false;
    }

    usb_test::post_control_event(*chip.controller, status_trb, XHCI_TRB_COMPLETION_CODE_STALL_ERROR);
    drivers::xhci_hcd_test_access::process_events(*chip.controller->hcd);
    chip.next_trb = status_trb + 1;
    if (!wait_for_recovery_request(chip)) {
        return false;
    }

    chip.is_recovered.store_release(0);
    sched::task* step = test_helpers::start_pinned_task_on_another_cpu(run_recovery_step, &chip, "chip_recovery", 0);
    if (!step) {
        return false;
    }

    // Reset Endpoint takes the entry a stop would, and the move after it sends EP0 past the refused request
    usb_test::complete_command(*chip.controller, chip.next_command, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    usb_test::answer_control_move(*chip.controller, chip.next_trb, chip.next_command + 1);
    chip.next_command += 2;

    bool is_recovered = test_helpers::spin_wait(chip.is_recovered);
    test_helpers::unpin(step);
    return is_recovered;
}

// Starts `calls` on a task of their own, or returns null when none could start
inline sched::task* start_calls(fake_chip& chip, void (*calls)()) {
    chip.calls = calls;
    chip.are_calls_done.store_release(0);
    return test_helpers::start_pinned_task_on_another_cpu(run_fake_chip_calls, &chip, "chip_calls", 0);
}

// Waits for the calls to end, false when they never do, which marks them stuck
inline bool finish_calls(fake_chip& chip, sched::task* task) {
    bool is_done = test_helpers::spin_wait(chip.are_calls_done);
    test_helpers::unpin(task);
    chip.is_call_stuck = chip.is_call_stuck || !is_done;
    return is_done;
}

// Makes `calls` and plays the chip for their `count` requests, answering each read with `reply`. False when a request
// goes unanswered or the calls never end
inline bool make_calls(
    fake_chip& chip,
    void (*calls)(),
    chip_request* requests,
    size_t count,
    const uint8_t* reply = nullptr,
    uint16_t reply_length = 0
) {
    sched::task* task = start_calls(chip, calls);
    if (!task) {
        return false;
    }

    bool is_answered = true;
    for (size_t i = 0; i < count && is_answered; i++) {
        string::memset(&requests[i], 0, sizeof(requests[i]));
        is_answered = answer_request(chip, requests[i], reply, reply_length);
    }

    return finish_calls(chip, task) && is_answered;
}

// Makes `calls` and plays the chip for their `count` requests, answering read `i` with `replies[i]`. False when a
// request goes unanswered or the calls never end
inline bool make_calls_with_replies(
    fake_chip& chip,
    void (*calls)(),
    chip_request* requests,
    const chip_reply* replies,
    size_t count
) {
    sched::task* task = start_calls(chip, calls);
    if (!task) {
        return false;
    }

    bool is_answered = true;
    for (size_t i = 0; i < count && is_answered; i++) {
        string::memset(&requests[i], 0, sizeof(requests[i]));
        is_answered = answer_request(chip, requests[i], replies[i].bytes, replies[i].length);
    }

    return finish_calls(chip, task) && is_answered;
}

// True when nothing has been queued since the last answered request
inline bool sent_nothing(const fake_chip& chip) {
    return usb_test::control_trb(*chip.controller, chip.next_trb) == nullptr;
}

} // namespace serial_test

#endif // STELLUX_TESTS_SERIAL_FAKE_CHIP_H
