#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/usb/fake_xhci.h"

TEST_SUITE(usb_in_stream);

using namespace usb_test;
using drivers::xhci::xhci_normal_trb_t;
using drivers::xhci::xhci_stop_endpoint_command_trb_t;

static constexpr uint32_t PAYLOAD_LENGTH        = 512;
static constexpr uint32_t READ_COUNT            = 2;
static constexpr uint32_t BYTES_BEFORE_STOP     = 64;
static constexpr uint32_t INTERRUPT_QUEUE_DEPTH = 2;

static constexpr uint8_t  INTERRUPT_IN           = 0x83;
static constexpr uint8_t  INTERRUPT_ATTRIBUTES   = 3;
static constexpr uint16_t INTERRUPT_MAX_PACKET   = 8;
static constexpr uint8_t  ISOCHRONOUS_IN         = 0x84;
static constexpr uint8_t  ISOCHRONOUS_ATTRIBUTES = 1;
static constexpr uint8_t  BULK_OUT               = 0x02;

static constexpr uint32_t MAX_CALLS = 8;
static constexpr uint32_t MAX_READS = 16;

// What one call of the stream's callback saw
struct stream_call {
    void*                context;
    usb::transfer_status status;
    const uint8_t*       data;
    uint32_t             length;
    uint8_t              last_byte;
    uint32_t             reads_posted; // ring entries written when the call came, finished reads included
    bool                 was_doorbell_rung;
    bool                 came_after_close;
};

// File scope, so a call the HCD task makes after a failed test never writes into a stack frame that has returned
static fake_xhci g_fake;
static xhci_endpoint* g_interrupt_in;
static xhci_endpoint* g_stream_endpoint;
static usb::in_stream* g_stream;
static uint8_t g_context;
static stream_call g_calls[MAX_CALLS];
static sync::atomic<uint32_t> g_call_count;
static sched::task* g_closer;
static sched::task* g_stop_step;
static sync::atomic<uint32_t> g_close_done;
static sync::atomic<uint32_t> g_stop_step_done;

static int32_t attach_controller() {
    string::memset(g_calls, 0, sizeof(g_calls));
    g_call_count.store_relaxed(0);
    g_stream = nullptr;

    g_closer = nullptr;
    g_stop_step = nullptr;
    g_close_done.store_relaxed(0);
    g_stop_step_done.store_relaxed(0);

    string::memset(&g_fake, 0, sizeof(g_fake));
    if (!attach_fake_xhci(g_fake)) {
        return -1;
    }

    g_stream_endpoint = g_fake.endpoint;
    g_interrupt_in = add_fake_endpoint(g_fake, INTERRUPT_IN, INTERRUPT_ATTRIBUTES, INTERRUPT_MAX_PACKET);
    bool has_more_endpoints =
        g_interrupt_in &&
        add_fake_endpoint(g_fake, ISOCHRONOUS_IN, ISOCHRONOUS_ATTRIBUTES, fake_xhci::MAX_PACKET) &&
        add_fake_endpoint(g_fake, BULK_OUT, fake_xhci::BULK_ATTRIBUTES, fake_xhci::MAX_PACKET);
    return has_more_endpoints ? 0 : -1;
}

// Starts `entry` on a task of its own on the CPU `cpu_offset` after the test runner's, lowered as the HCD task and
// class drivers' tasks are, so it runs while the runner spins
static sched::task* start_task(void (*entry)(void*), const char* name, uint32_t cpu_offset = 1) {
    sched::task* task = nullptr;
    RUN_ELEVATED({
        uint32_t cpu = (percpu::current_cpu_id() + cpu_offset) % smp::cpu_count();
        task = sched::create_kernel_task(entry, nullptr, name, 0);
        if (task) {
            task->add_ref();
            sched::enqueue_on(task, cpu);
        }
    });

    return task;
}

// Runs the close, then marks it done and ends its task
static void run_close(void*) {
    usb::close_in_stream(g_stream);
    g_close_done.store_release(1);
    sched::exit(0);
}

// The close a driver's probe or disconnect makes, on the HCD task
static void run_close_on_hcd_task(void*) {
    xhci_hcd_test_access::set_task(*g_fake.hcd, sched::current());
    usb::close_in_stream(g_stream);
    xhci_hcd_test_access::set_task(*g_fake.hcd, nullptr);
    g_close_done.store_release(1);
    sched::exit(0);
}

// Runs the step, then marks it done and ends its task
static void run_stop_step(void*) {
    xhci_hcd_test_access::stop_requested_endpoints(*g_fake.hcd);
    g_stop_step_done.store_release(1);
    sched::exit(0);
}

// Answers the next command the driver wrote with success and no transfer event
static void answer_next_command() {
    const volatile xhci_trb_t* entry = &g_fake.command_ring->get_virtual_base()[g_fake.commands_completed];
    if (entry->control & fake_xhci::CYCLE_BIT) {
        post_command_completion(g_fake, g_fake.commands_completed, XHCI_TRB_COMPLETION_CODE_SUCCESS);
        g_fake.commands_completed++;
    }
}

// Closes a stream the test left open, starting the stop step once the close waits and answering each command. A task
// still running at the deadline keeps the fake, which is then never freed, so it never wakes in freed memory.
static int32_t close_stream_and_detach() {
    if (g_stream && !g_closer) {
        g_closer = start_task(run_close, "usb_stream_close");
    }

    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (g_closer && !g_close_done.load_acquire() && clock::now_ns() < deadline) {
        if (!g_stop_step && g_closer->state.load_acquire() == sched::TASK_STATE_BLOCKED) {
            g_stop_step = start_task(run_stop_step, "usb_stream_stop", 2);
        }

        answer_next_command();
        raise_interrupt(g_fake);
        test_helpers::brief_delay();
    }

    while (g_stop_step && !g_stop_step_done.load_acquire() && clock::now_ns() < deadline) {
        answer_next_command();
        raise_interrupt(g_fake);
        test_helpers::brief_delay();
    }

    bool is_closer_running = g_closer && !g_close_done.load_acquire();
    bool is_step_running = g_stop_step && !g_stop_step_done.load_acquire();
    if (is_closer_running || is_step_running) {
        return -1;
    }

    if (g_closer) {
        test_helpers::unpin(g_closer);
    }

    if (g_stop_step) {
        test_helpers::unpin(g_stop_step);
    }

    detach_fake_xhci(g_fake);
    return 0;
}

BEFORE_EACH(usb_in_stream, attach_controller);
AFTER_EACH(usb_in_stream, close_stream_and_detach);

// Records each call with what the ring and the doorbell held by then
static void record_call(void* context, const uint8_t* data, uint32_t length, usb::transfer_status status) {
    uint32_t index = g_call_count.load_relaxed();
    if (index < MAX_CALLS) {
        stream_call& call = g_calls[index];
        call.context = context;
        call.status = status;

        call.data = data;
        call.length = length;
        call.last_byte = data && length > 0 ? data[length - 1] : 0;

        while (call.reads_posted < MAX_READS && transfer_trb(g_fake, call.reads_posted, g_stream_endpoint)) {
            call.reads_posted++;
        }

        call.was_doorbell_rung = g_fake.doorbells[fake_xhci::SLOT_ID] == g_stream_endpoint->dci();
        call.came_after_close = g_close_done.load_acquire() != 0;
    }

    g_call_count.store_release(index + 1);
}

// Opens a bulk IN stream of `read_count` reads of `payload_length` bytes that records each call
static int32_t open_bulk_stream(uint32_t read_count = READ_COUNT, uint32_t payload_length = PAYLOAD_LENGTH) {
    return usb::open_in_stream(
        &g_fake.usb_device, fake_xhci::BULK_IN, payload_length, read_count, record_call, &g_context, &g_stream);
}

// The buffer address the read at ring entry `index` of `endpoint` was posted with, or 0 when no read is posted there
static uint64_t read_buffer_phys(xhci_endpoint* endpoint, uint32_t index) {
    auto* read = reinterpret_cast<const xhci_normal_trb_t*>(transfer_trb(g_fake, index, endpoint));
    return read ? read->data_buffer_physical_base : 0;
}

// The endpoint's DMA buffer or the stream buffer at `phys`, where the controller writes a read posted there
static void* buffer_at(xhci_endpoint* endpoint, uint64_t phys) {
    uint32_t read_count = endpoint->async_state()->in_stream.read_count;
    for (uint32_t read = 0; read < read_count; read++) {
        if (endpoint->stream_buffer_phys(read) == phys) {
            return endpoint->stream_buffer(read);
        }
    }

    return endpoint->dma_buffer();
}

// Plays the controller finishing the read at ring entry `index` of `endpoint` with `length` bytes of `byte`, written
// into the buffer the read was posted with
static void finish_read(
    xhci_endpoint* endpoint,
    uint32_t payload_length,
    uint32_t index,
    uint32_t length,
    uint8_t byte
) {
    uint64_t buffer_phys = read_buffer_phys(endpoint, index);
    if (buffer_phys != 0) {
        string::memset(buffer_at(endpoint, buffer_phys), byte, length);
    }

    uint8_t completion_code = length == payload_length
        ? XHCI_TRB_COMPLETION_CODE_SUCCESS
        : XHCI_TRB_COMPLETION_CODE_SHORT_PACKET;
    post_transfer_event(g_fake, index, payload_length - length, completion_code, endpoint);
}

// Finishes the read as `finish_read` does, then runs the HCD task's event processing, which makes its call
static void complete_read(
    xhci_endpoint* endpoint,
    uint32_t payload_length,
    uint32_t index,
    uint32_t length,
    uint8_t byte
) {
    finish_read(endpoint, payload_length, index, length, byte);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
}

// The read at entry 1 is still pending when entry 0 completes, and entry 0's buffer is posted again only once its call
// has returned, so the controller never writes the buffer a call reads
TEST(usb_in_stream, each_read_is_posted_again_after_its_call_while_the_other_is_pending) {
    ASSERT_EQ(open_bulk_stream(), 0);

    auto* first = reinterpret_cast<const xhci_normal_trb_t*>(transfer_trb(g_fake, 0));
    ASSERT_NOT_NULL(first);
    EXPECT_EQ(first->trb_transfer_length, PAYLOAD_LENGTH);
    EXPECT_EQ(g_fake.doorbells[fake_xhci::SLOT_ID], static_cast<uint32_t>(g_fake.endpoint->dci()));

    EXPECT_EQ(read_buffer_phys(g_fake.endpoint, 0), g_fake.endpoint->stream_buffer_phys(0));
    EXPECT_EQ(read_buffer_phys(g_fake.endpoint, 1), g_fake.endpoint->stream_buffer_phys(1));
    EXPECT_NULL(transfer_trb(g_fake, 2));

    complete_read(g_fake.endpoint, PAYLOAD_LENGTH, 0, 10, 'a');
    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(g_calls[0].context, &g_context);
    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::short_packet));

    EXPECT_EQ(g_calls[0].data, g_fake.endpoint->stream_buffer(0));
    EXPECT_EQ(g_calls[0].length, 10u);
    EXPECT_EQ(g_calls[0].last_byte, 'a');
    EXPECT_EQ(g_calls[0].reads_posted, 2u);
    EXPECT_EQ(read_buffer_phys(g_fake.endpoint, 2), g_fake.endpoint->stream_buffer_phys(0));

    complete_read(g_fake.endpoint, PAYLOAD_LENGTH, 1, PAYLOAD_LENGTH, 'b');
    ASSERT_EQ(g_call_count.load_acquire(), 2u);
    EXPECT_EQ(code_of(g_calls[1].status), code_of(usb::transfer_status::ok));
    EXPECT_EQ(g_calls[1].data, g_fake.endpoint->stream_buffer(1));
    EXPECT_EQ(g_calls[1].last_byte, 'b');

    EXPECT_EQ(g_calls[1].reads_posted, 3u);
    EXPECT_EQ(read_buffer_phys(g_fake.endpoint, 3), g_fake.endpoint->stream_buffer_phys(1));
}

// Both reads finish before the HCD task gets a CPU, and the first read is posted again before the second's call
TEST(usb_in_stream, calls_come_in_completion_order) {
    ASSERT_EQ(open_bulk_stream(), 0);
    g_fake.doorbells[fake_xhci::SLOT_ID] = 0;

    finish_read(g_fake.endpoint, PAYLOAD_LENGTH, 0, 1, 'a');
    finish_read(g_fake.endpoint, PAYLOAD_LENGTH, 1, 2, 'b');
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    ASSERT_EQ(g_call_count.load_acquire(), 2u);

    EXPECT_EQ(g_calls[0].length, 1u);
    EXPECT_EQ(g_calls[0].last_byte, 'a');
    EXPECT_FALSE(g_calls[0].was_doorbell_rung);

    EXPECT_EQ(g_calls[1].length, 2u);
    EXPECT_EQ(g_calls[1].last_byte, 'b');
    EXPECT_EQ(g_calls[1].reads_posted, 3u);
    EXPECT_TRUE(g_calls[1].was_doorbell_rung);
    EXPECT_EQ(read_buffer_phys(g_fake.endpoint, 3), g_fake.endpoint->stream_buffer_phys(1));
}

static constexpr uint32_t LAP_READS = 4;

// Five reads complete on a stream of four, so the fifth call reads the first buffer again, which is posted once more
TEST(usb_in_stream, each_read_keeps_its_buffer_across_a_lap_of_the_stream) {
    ASSERT_EQ(open_bulk_stream(LAP_READS), 0);
    for (uint32_t read = 0; read < LAP_READS; read++) {
        EXPECT_EQ(read_buffer_phys(g_fake.endpoint, read), g_fake.endpoint->stream_buffer_phys(read));
    }

    for (uint32_t read = 0; read <= LAP_READS; read++) {
        complete_read(g_fake.endpoint, PAYLOAD_LENGTH, read, read + 1, static_cast<uint8_t>('a' + read));
    }

    ASSERT_EQ(g_call_count.load_acquire(), LAP_READS + 1);
    EXPECT_EQ(g_calls[LAP_READS].data, g_fake.endpoint->stream_buffer(0));
    EXPECT_EQ(g_calls[LAP_READS].last_byte, 'a' + LAP_READS);
    EXPECT_EQ(read_buffer_phys(g_fake.endpoint, 2 * LAP_READS), g_fake.endpoint->stream_buffer_phys(0));
}

// The controller could not service the read at entry 0 within the interrupt endpoint's interval, and the read is posted
// again with no call
TEST(usb_in_stream, a_read_that_missed_its_service_is_posted_again_without_a_call) {
    g_stream_endpoint = g_interrupt_in;
    ASSERT_EQ(usb::open_in_stream(
        &g_fake.usb_device, INTERRUPT_IN, INTERRUPT_MAX_PACKET, READ_COUNT, record_call, &g_context, &g_stream), 0);

    post_transfer_event(g_fake, 0, INTERRUPT_MAX_PACKET, XHCI_TRB_COMPLETION_CODE_MISSED_SERVICE, g_interrupt_in);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    EXPECT_EQ(g_call_count.load_acquire(), 0u);
    EXPECT_EQ(read_buffer_phys(g_interrupt_in, 2), g_interrupt_in->stream_buffer_phys(0));

    complete_read(g_interrupt_in, INTERRUPT_MAX_PACKET, 1, 3, 'k');
    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(g_calls[0].data, g_interrupt_in->stream_buffer(1));
    EXPECT_EQ(g_calls[0].last_byte, 'k');
}

// An event for an entry no read was posted at, and a second event for a read already finished, make no call and leave
// the next call on the right buffer
TEST(usb_in_stream, an_event_for_no_posted_read_is_dropped) {
    ASSERT_EQ(open_bulk_stream(), 0);

    post_transfer_event(g_fake, 7, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    EXPECT_EQ(g_call_count.load_acquire(), 0u);
    EXPECT_NULL(transfer_trb(g_fake, 2));

    complete_read(g_fake.endpoint, PAYLOAD_LENGTH, 0, 1, 'a');
    post_transfer_event(g_fake, 0, PAYLOAD_LENGTH - 1, XHCI_TRB_COMPLETION_CODE_SHORT_PACKET);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    ASSERT_EQ(g_call_count.load_acquire(), 1u);

    complete_read(g_fake.endpoint, PAYLOAD_LENGTH, 1, 2, 'b');
    ASSERT_EQ(g_call_count.load_acquire(), 2u);
    EXPECT_EQ(g_calls[1].data, g_fake.endpoint->stream_buffer(1));
    EXPECT_EQ(g_calls[1].last_byte, 'b');
}

// Closes the stream on a task of its own and waits for the close, which needs no stop step
static bool close_without_stop() {
    g_closer = start_task(run_close, "usb_stream_close");
    if (!g_closer || !test_helpers::spin_wait(g_close_done)) {
        return false;
    }

    g_stream = nullptr;
    return true;
}

static usb::transfer_request g_request;
static uint8_t g_request_buffer[PAYLOAD_LENGTH];

static bool can_submit_read() {
    usb::init_transfer_request(g_request, fake_xhci::BULK_IN, g_request_buffer, PAYLOAD_LENGTH, usb::allow_short);
    return usb::submit_transfer_async(&g_fake.usb_device, g_request) == 0;
}

// The failed read leaves the other read on the ring, so the endpoint takes no new stream or transfer
TEST(usb_in_stream, a_failed_read_makes_the_streams_last_call_and_retires_its_endpoint) {
    ASSERT_EQ(open_bulk_stream(), 0);

    post_transfer_event(g_fake, 0, PAYLOAD_LENGTH, XHCI_TRB_COMPLETION_CODE_STALL_ERROR);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::stalled));
    EXPECT_EQ(g_calls[0].length, 0u);
    EXPECT_NULL(transfer_trb(g_fake, 2));

    EXPECT_NE(open_bulk_stream(), 0);
    EXPECT_FALSE(can_submit_read());

    ASSERT_TRUE(close_without_stop());
    EXPECT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(g_fake.command_ring->get_virtual_base()[STOP_COMMAND].control, 0u);
    EXPECT_NE(open_bulk_stream(), 0);
    EXPECT_FALSE(can_submit_read());
}

// A refused open must leave the live stream's buffers, their stride and its calls as they were
TEST(usb_in_stream, a_second_open_leaves_the_live_streams_buffers_alone) {
    ASSERT_EQ(open_bulk_stream(), 0);
    usb::in_stream* live = g_stream;
    uint64_t first_buffer = g_fake.endpoint->stream_buffer_phys(0);
    uint64_t stride = g_fake.endpoint->stream_buffer_phys(1) - first_buffer;

    EXPECT_NE(open_bulk_stream(READ_COUNT * 4, PAYLOAD_LENGTH * 2), 0);
    EXPECT_NE(open_bulk_stream(READ_COUNT, fake_xhci::MAX_PACKET), 0);
    g_stream = live;
    EXPECT_EQ(g_fake.endpoint->stream_buffer_phys(0), first_buffer);
    EXPECT_EQ(g_fake.endpoint->stream_buffer_phys(1) - first_buffer, stride);

    complete_read(g_fake.endpoint, PAYLOAD_LENGTH, 1, PAYLOAD_LENGTH, 'b');
    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(g_calls[0].data, g_fake.endpoint->stream_buffer(1));
    EXPECT_EQ(g_calls[0].last_byte, 'b');
}

// Starts a close on a task of its own and, once the close waits, the HCD task's step that stops the reads on another
// CPU, so a close that returned too early would show while the step still runs
static bool start_close() {
    g_closer = start_task(run_close, "usb_stream_close");
    if (!g_closer || !test_helpers::blocks_before_deadline(g_closer)) {
        return false;
    }

    g_stop_step = start_task(run_stop_step, "usb_stream_stop", 2);
    return g_stop_step != nullptr;
}

// Waits for a Stop Endpoint command for the bulk IN endpoint in command ring entry 0, and gives whether one came
static bool wait_for_stop() {
    auto* stop = reinterpret_cast<const xhci_stop_endpoint_command_trb_t*>(wait_for_command(g_fake, STOP_COMMAND));
    return stop && stop->trb_type == XHCI_TRB_TYPE_STOP_ENDPOINT_CMD && stop->endpoint_id == g_fake.endpoint->dci();
}

// The first read finishes before the stop and is not posted again, so a second event about it makes no call. The second
// read is stopped with bytes in it, and the close waits for the dequeue move after its call.
TEST(usb_in_stream, a_close_returns_after_the_calls_for_both_reads) {
    ASSERT_EQ(open_bulk_stream(), 0);
    ASSERT_TRUE(start_close());

    ASSERT_TRUE(wait_for_stop());
    finish_read(g_fake.endpoint, PAYLOAD_LENGTH, 0, 20, 'a');
    post_transfer_event(g_fake, 0, PAYLOAD_LENGTH - 20, XHCI_TRB_COMPLETION_CODE_SHORT_PACKET);
    string::memset(g_fake.endpoint->stream_buffer(1), 'b', BYTES_BEFORE_STOP);
    post_transfer_event(g_fake, 1, PAYLOAD_LENGTH - BYTES_BEFORE_STOP, XHCI_TRB_COMPLETION_CODE_STOPPED);
    complete_command(g_fake, STOP_COMMAND, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    ASSERT_NOT_NULL(wait_for_command(g_fake, MOVE_COMMAND));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(g_stop_step));
    test_helpers::brief_delay();
    EXPECT_EQ(g_close_done.load_acquire(), 0u);

    answer_move(g_fake, 2);
    ASSERT_TRUE(test_helpers::spin_wait(g_close_done));
    g_stream = nullptr;

    ASSERT_EQ(g_call_count.load_acquire(), 2u);
    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::short_packet));
    EXPECT_EQ(g_calls[0].length, 20u);
    EXPECT_EQ(g_calls[0].last_byte, 'a');

    EXPECT_EQ(code_of(g_calls[1].status), code_of(usb::transfer_status::cancelled));
    EXPECT_EQ(g_calls[1].data, g_fake.endpoint->stream_buffer(1));
    EXPECT_EQ(g_calls[1].length, BYTES_BEFORE_STOP);
    EXPECT_EQ(g_calls[1].last_byte, 'b');
    EXPECT_FALSE(g_calls[1].came_after_close);
}

static sync::atomic<uint32_t> g_is_last_call_held;
static sync::atomic<uint32_t> g_release_last_call;

// Records each call, and holds the last one until the test releases it
static void hold_last_call(void* context, const uint8_t* data, uint32_t length, usb::transfer_status status) {
    record_call(context, data, length, status);
    if (status == usb::transfer_status::ok || status == usb::transfer_status::short_packet) {
        return;
    }

    g_is_last_call_held.store_release(1);
    (void)test_helpers::spin_wait(g_release_last_call);
}

// The stop brings no event for either read, so the stop step makes the last call itself, and the close waits for it
TEST(usb_in_stream, a_close_whose_stop_brings_no_event_returns_after_the_last_call) {
    g_is_last_call_held.store_relaxed(0);
    g_release_last_call.store_relaxed(0);
    ASSERT_EQ(usb::open_in_stream(
        &g_fake.usb_device, fake_xhci::BULK_IN, PAYLOAD_LENGTH, READ_COUNT, hold_last_call, &g_context, &g_stream),
        0);
    ASSERT_TRUE(start_close());

    ASSERT_TRUE(wait_for_stop());
    complete_command(g_fake, STOP_COMMAND, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    answer_move(g_fake, 2);
    ASSERT_TRUE(test_helpers::spin_wait(g_is_last_call_held));
    test_helpers::brief_delay();
    EXPECT_EQ(g_close_done.load_acquire(), 0u);

    g_release_last_call.store_release(1);
    ASSERT_TRUE(test_helpers::spin_wait(g_close_done));
    g_stream = nullptr;

    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::cancelled));
    EXPECT_NULL(g_calls[0].data);
    EXPECT_EQ(g_calls[0].length, 0u);
}

// The controller refuses the close's stop, so the reads may still run, and the endpoint takes no new stream even with
// buffers its old ones could not hold
TEST(usb_in_stream, a_close_whose_stop_is_refused_ends_the_stream_with_an_error) {
    ASSERT_EQ(open_bulk_stream(), 0);
    uint64_t first_buffer = g_fake.endpoint->stream_buffer_phys(0);
    ASSERT_TRUE(start_close());

    complete_command(g_fake, STOP_COMMAND, XHCI_TRB_COMPLETION_CODE_CONTEXT_STATE_ERROR);
    ASSERT_TRUE(test_helpers::spin_wait(g_close_done));
    g_stream = nullptr;

    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::io_error));
    EXPECT_FALSE(g_calls[0].came_after_close);
    EXPECT_NE(open_bulk_stream(READ_COUNT * 4, PAYLOAD_LENGTH * 2), 0);
    EXPECT_EQ(g_fake.endpoint->stream_buffer_phys(0), first_buffer);
}

// A probe that fails after opening its stream closes it on the HCD task, and a read stopped with the invalid length
// code reports no bytes, whatever the event's length field holds
TEST(usb_in_stream, a_close_on_the_hcd_task_stops_the_reads_itself) {
    ASSERT_EQ(open_bulk_stream(), 0);
    g_closer = start_task(run_close_on_hcd_task, "usb_stream_close");
    ASSERT_NOT_NULL(g_closer);

    ASSERT_TRUE(wait_for_stop());
    post_transfer_event(g_fake, 0, 0, XHCI_TRB_COMPLETION_CODE_STOPPED_LENGTH_INVALID);
    complete_command(g_fake, STOP_COMMAND, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    answer_move(g_fake, 2);
    ASSERT_TRUE(test_helpers::spin_wait(g_close_done));
    g_stream = nullptr;

    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::cancelled));
    EXPECT_EQ(g_calls[0].length, 0u);
    EXPECT_FALSE(g_calls[0].came_after_close);
}

static bool g_was_close_waiting;

// Starts a close from inside the call, and returns once the close waits for the call to return
static void close_during_call(void* context, const uint8_t* data, uint32_t length, usb::transfer_status status) {
    record_call(context, data, length, status);
    g_closer = start_task(run_close, "usb_stream_close");
    g_was_close_waiting = g_closer && test_helpers::blocks_before_deadline(g_closer);
}

// A failed read stops nothing, so the call's return is what lets the close end
TEST(usb_in_stream, a_close_during_the_last_call_returns_once_the_call_has_run) {
    g_was_close_waiting = false;
    ASSERT_EQ(usb::open_in_stream(
        &g_fake.usb_device, fake_xhci::BULK_IN, PAYLOAD_LENGTH, READ_COUNT, close_during_call, &g_context, &g_stream),
        0);

    post_transfer_event(g_fake, 0, PAYLOAD_LENGTH, XHCI_TRB_COMPLETION_CODE_USB_TRANSACTION_ERROR);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    EXPECT_TRUE(g_was_close_waiting);
    ASSERT_TRUE(test_helpers::spin_wait(g_close_done));
    g_stream = nullptr;

    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::io_error));
    EXPECT_EQ(g_fake.command_ring->get_virtual_base()[STOP_COMMAND].control, 0u);
}

// A read that finishes once the device is marked gone still gets its call, and is not posted again
TEST(usb_in_stream, an_unplug_makes_the_streams_last_call) {
    ASSERT_EQ(open_bulk_stream(), 0);

    xhci_hcd_test_access::mark_unplugged(*g_fake.hcd, g_fake.device);
    complete_read(g_fake.endpoint, PAYLOAD_LENGTH, 0, 1, 'a');
    EXPECT_NULL(transfer_trb(g_fake, 2));

    xhci_hcd_test_access::end_unplugged_transfers(*g_fake.hcd, g_fake.device);
    ASSERT_EQ(g_call_count.load_acquire(), 2u);
    EXPECT_EQ(g_calls[0].last_byte, 'a');
    EXPECT_EQ(code_of(g_calls[1].status), code_of(usb::transfer_status::device_gone));
    EXPECT_NULL(g_calls[1].data);

    ASSERT_TRUE(close_without_stop());
    EXPECT_EQ(g_call_count.load_acquire(), 2u);
    EXPECT_EQ(g_fake.command_ring->get_virtual_base()[STOP_COMMAND].control, 0u);
}

// A driver's task closes its stream after the teardown has marked the device gone and before it ends the transfers
TEST(usb_in_stream, a_close_during_an_unplug_waits_for_the_unplugs_last_call) {
    ASSERT_EQ(open_bulk_stream(), 0);
    xhci_hcd_test_access::mark_unplugged(*g_fake.hcd, g_fake.device);

    g_closer = start_task(run_close, "usb_stream_close");
    ASSERT_NOT_NULL(g_closer);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(g_closer));

    xhci_hcd_test_access::end_unplugged_transfers(*g_fake.hcd, g_fake.device);
    ASSERT_TRUE(test_helpers::spin_wait(g_close_done));
    g_stream = nullptr;

    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::device_gone));
    EXPECT_FALSE(g_calls[0].came_after_close);
    EXPECT_EQ(g_fake.command_ring->get_virtual_base()[STOP_COMMAND].control, 0u);
}

// A driver's disconnect runs on the HCD task between the teardown's two steps, and may close its stream there
TEST(usb_in_stream, a_close_from_a_disconnect_makes_the_last_call_itself) {
    ASSERT_EQ(open_bulk_stream(), 0);
    xhci_hcd_test_access::mark_unplugged(*g_fake.hcd, g_fake.device);

    g_closer = start_task(run_close_on_hcd_task, "usb_stream_close");
    ASSERT_NOT_NULL(g_closer);
    ASSERT_TRUE(test_helpers::spin_wait(g_close_done));
    g_stream = nullptr;

    xhci_hcd_test_access::end_unplugged_transfers(*g_fake.hcd, g_fake.device);
    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::device_gone));
    EXPECT_FALSE(g_calls[0].came_after_close);
    EXPECT_EQ(g_fake.command_ring->get_virtual_base()[STOP_COMMAND].control, 0u);
}

static uint8_t g_read_buffer[PAYLOAD_LENGTH];
static sync::atomic<uint32_t> g_read_done;
static int32_t g_read_result;

// Reads from the stream, then records the result, marks it done and ends its task
static void run_read(void*) {
    uint32_t length = 0;
    g_read_result = usb::read_in_stream(g_stream, g_read_buffer, sizeof(g_read_buffer), &length);
    g_read_done.store_release(1);
    sched::exit(0);
}

// On a task of its own, since a read that waited would never end
TEST(usb_in_stream, a_stream_with_a_callback_has_nothing_to_read) {
    ASSERT_EQ(open_bulk_stream(), 0);

    g_read_done.store_relaxed(0);
    sched::task* reader = start_task(run_read, "usb_stream_read");
    ASSERT_NOT_NULL(reader);
    EXPECT_TRUE(test_helpers::spin_wait(g_read_done));
    EXPECT_NE(g_read_result, 0);
    test_helpers::unpin(reader);
}

// Reads a payload only while one is queued, since a read of an empty live stream waits and the test runner must not
static bool read_queued_payload(xhci_endpoint* endpoint, uint32_t* length) {
    if (endpoint->async_state()->in_stream.count == 0) {
        return false;
    }

    return usb::read_in_stream(g_stream, g_read_buffer, sizeof(g_read_buffer), length) == 0;
}

// HID's stream keeps its newest two payloads, and its close stops the one posted read and ends a read waiting on the
// empty queue
TEST(usb_in_stream, a_stream_without_a_callback_queues_its_newest_two_payloads_until_closed) {
    ASSERT_EQ(usb::open_in_stream(
        &g_fake.usb_device, INTERRUPT_IN, INTERRUPT_MAX_PACKET, 1, nullptr, nullptr, &g_stream), 0);
    EXPECT_NULL(transfer_trb(g_fake, 1, g_interrupt_in));

    uint32_t completed_reads = INTERRUPT_QUEUE_DEPTH + 1;
    for (uint32_t i = 0; i < completed_reads; i++) {
        complete_read(g_interrupt_in, INTERRUPT_MAX_PACKET, i, i + 1, static_cast<uint8_t>('a' + i));
    }

    uint32_t length = 0;
    ASSERT_TRUE(read_queued_payload(g_interrupt_in, &length));
    EXPECT_EQ(length, 2u);
    EXPECT_EQ(g_read_buffer[1], 'b');

    ASSERT_TRUE(read_queued_payload(g_interrupt_in, &length));
    EXPECT_EQ(length, 3u);
    EXPECT_EQ(g_interrupt_in->async_state()->in_stream.count, 0u);

    g_read_done.store_relaxed(0);
    sched::task* reader = start_task(run_read, "usb_stream_read", 3);
    ASSERT_NOT_NULL(reader);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(reader));
    ASSERT_TRUE(start_close());

    auto* stop = reinterpret_cast<const xhci_stop_endpoint_command_trb_t*>(wait_for_command(g_fake, STOP_COMMAND));
    ASSERT_NOT_NULL(stop);
    EXPECT_EQ(stop->endpoint_id, g_interrupt_in->dci());
    complete_command(g_fake, STOP_COMMAND, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    answer_move(g_fake, completed_reads + 1, FIRST_LAP_CYCLE, g_interrupt_in);

    ASSERT_TRUE(test_helpers::spin_wait(g_close_done));
    g_stream = nullptr;
    EXPECT_TRUE(test_helpers::spin_wait(g_read_done));
    EXPECT_NE(g_read_result, 0);
    test_helpers::unpin(reader);
}

// A bulk read one byte past a whole number of packets is refused, while an interrupt read short of a packet is not, and
// a bulk endpoint that names no packet size takes no stream
TEST(usb_in_stream, a_bulk_stream_needs_a_callback_and_whole_packets) {
    EXPECT_NE(usb::open_in_stream(
        &g_fake.usb_device, fake_xhci::BULK_IN, PAYLOAD_LENGTH, 1, nullptr, nullptr, &g_stream), 0);
    EXPECT_NE(open_bulk_stream(READ_COUNT, fake_xhci::MAX_PACKET + 1), 0);

    static constexpr uint8_t SIZELESS_BULK_IN = 0x85;
    ASSERT_NOT_NULL(add_fake_endpoint(g_fake, SIZELESS_BULK_IN, fake_xhci::BULK_ATTRIBUTES, 0));
    EXPECT_NE(usb::open_in_stream(
        &g_fake.usb_device, SIZELESS_BULK_IN, PAYLOAD_LENGTH, READ_COUNT, record_call, &g_context, &g_stream), 0);
    EXPECT_NULL(g_stream);

    EXPECT_EQ(usb::open_in_stream(
        &g_fake.usb_device, INTERRUPT_IN, INTERRUPT_MAX_PACKET - 1, 1, nullptr, nullptr, &g_stream), 0);
}

TEST(usb_in_stream, streams_open_only_on_interrupt_and_bulk_in_endpoints) {
    EXPECT_NE(usb::open_in_stream(
        &g_fake.usb_device, BULK_OUT, PAYLOAD_LENGTH, READ_COUNT, record_call, &g_context, &g_stream), 0);
    EXPECT_NE(usb::open_in_stream(
        &g_fake.usb_device, ISOCHRONOUS_IN, PAYLOAD_LENGTH, READ_COUNT, record_call, &g_context, &g_stream), 0);
    EXPECT_NULL(g_stream);
}

// A stream with a callback keeps 2 to `usb::MAX_IN_STREAM_READS` reads posted, and one without keeps one
TEST(usb_in_stream, a_stream_opens_only_with_a_read_count_its_kind_allows) {
    EXPECT_NE(open_bulk_stream(1), 0);
    EXPECT_NE(open_bulk_stream(usb::MAX_IN_STREAM_READS + 1), 0);
    EXPECT_NE(usb::open_in_stream(
        &g_fake.usb_device, INTERRUPT_IN, INTERRUPT_MAX_PACKET, 0, nullptr, nullptr, &g_stream), 0);
    EXPECT_NE(usb::open_in_stream(
        &g_fake.usb_device, INTERRUPT_IN, INTERRUPT_MAX_PACKET, READ_COUNT, nullptr, nullptr, &g_stream), 0);
    EXPECT_NULL(g_stream);

    ASSERT_EQ(open_bulk_stream(usb::MAX_IN_STREAM_READS), 0);
    EXPECT_NOT_NULL(transfer_trb(g_fake, usb::MAX_IN_STREAM_READS - 1));
    EXPECT_NULL(transfer_trb(g_fake, usb::MAX_IN_STREAM_READS));
}

static constexpr uint32_t SMALL_READS   = 32;
static constexpr uint32_t THREE_PACKETS = 3 * fake_xhci::MAX_PACKET;
static constexpr uint32_t PAGE_BYTES    = 4096;

// A read of three packets gets a buffer of a power of two in size, so none of 32 crosses a page
TEST(usb_in_stream, no_stream_buffer_crosses_a_page) {
    ASSERT_EQ(open_bulk_stream(SMALL_READS, THREE_PACKETS), 0);
    for (uint32_t read = 0; read < SMALL_READS; read++) {
        EXPECT_LE(g_fake.endpoint->stream_buffer_phys(read) % PAGE_BYTES + THREE_PACKETS, PAGE_BYTES);
    }
}

// A read of whole packets one packet past a page is refused, while one of exactly a page is not
TEST(usb_in_stream, a_read_spans_a_page_at_most) {
    EXPECT_NE(open_bulk_stream(READ_COUNT, PAGE_BYTES + fake_xhci::MAX_PACKET), 0);
    EXPECT_EQ(open_bulk_stream(READ_COUNT, PAGE_BYTES), 0);
}
