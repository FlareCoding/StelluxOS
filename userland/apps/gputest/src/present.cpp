// Presents on gfxfb's output 0 in stlxdm's place, from a thread of its own, while the GPU checks run

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stlx/gfxfb_abi.h>

extern "C" {
#include "gputest.h"
}

constexpr uint32_t PRESENT_OUTPUT = 0;
constexpr uint32_t BUFFER_COUNT   = 2;

// Each present names one small region, so a device that copies what changed copies little
constexpr uint32_t REGION_PIXELS = 64;

constexpr uint64_t NS_PER_US            = 1000;
constexpr uint64_t MILLIHERTZ_PER_HERTZ = 1000;

static int       gfxfb_fd = -1;
static pthread_t presenter;
static bool      is_running = false;
static bool      is_stopping = false;
static uint64_t  ended = 0;
static uint64_t  missed = 0;          // presents not seen to latch within their deadline, refused ones included
static uint64_t  longest_call_ns = 0; // the longest GFXFB_PRESENT call, which can wait for the driver's batch lock
static uint64_t  period_ns = 0;
static uint32_t  handles[BUFFER_COUNT];

// Reads events until a present's end, past any output event. Returns false when a read fails
static bool read_present_end(gfxfb::gfxfb_event& event) {
    while (read(gfxfb_fd, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event))) {
        if (event.type == gfxfb::GFXFB_EVENT_PRESENT) {
            return true;
        }
    }

    return false;
}

static void* present_until_stopped(void*) {
    gfxfb::gfxfb_rect region = {0, 0, REGION_PIXELS, REGION_PIXELS};
    uint32_t next = 0;

    while (!__atomic_load_n(&is_stopping, __ATOMIC_ACQUIRE)) {
        gfxfb::gfxfb_present_args args = {};
        args.output = PRESENT_OUTPUT;
        args.handle = handles[next];
        args.regions = reinterpret_cast<uint64_t>(&region);
        args.region_count = 1;

        gfxfb::gfxfb_event event = {};
        uint64_t call_ns = monotonic_ns();
        bool presented = ioctl(gfxfb_fd, gfxfb::GFXFB_PRESENT, &args) == 0;
        uint64_t presented_ns = monotonic_ns();
        if (presented_ns - call_ns > longest_call_ns) {
            longest_call_ns = presented_ns - call_ns;
        }

        if (!presented || !read_present_end(event)) {
            missed++;
            break;
        }

        bool latched = event.status == gfxfb::GFXFB_PRESENT_LATCHED;
        bool on_time = event.timestamp_ns <= presented_ns + gfxfb::GFXFB_PRESENT_TIMEOUT_NS;
        if (!latched || !on_time) {
            missed++;
        }

        __atomic_add_fetch(&ended, 1, __ATOMIC_RELEASE);

        next = (next + 1) % BUFFER_COUNT;
    }

    return nullptr;
}

bool start_presenting(void) {
    gfxfb_fd = open("/dev/gfxfb", O_RDWR);

    gfxfb::gfxfb_output_info output = {};
    output.output = PRESENT_OUTPUT;
    bool has_output = gfxfb_fd >= 0 && ioctl(gfxfb_fd, gfxfb::GFXFB_GET_OUTPUT, &output) == 0;
    bool is_lit = has_output && (output.flags & gfxfb::GFXFB_OUTPUT_LIT) != 0;

    uint64_t refresh_millihertz = output.current.refresh_millihertz;
    period_ns = refresh_millihertz != 0 ? NS_PER_SEC * MILLIHERTZ_PER_HERTZ / refresh_millihertz : 0;

    bool has_buffers = is_lit;
    for (uint32_t i = 0; i < BUFFER_COUNT && has_buffers; i++) {
        gfxfb::gfxfb_buffer_args args = {};
        args.width = output.current.width;
        args.height = output.current.height;
        args.format = gfxfb::GFXFB_FORMAT_XRGB8888;

        has_buffers = ioctl(gfxfb_fd, gfxfb::GFXFB_CREATE_BUFFER, &args) == 0;
        handles[i] = args.handle;
    }

    is_running = has_buffers && pthread_create(&presenter, nullptr, present_until_stopped, nullptr) == 0;
    if (!is_running && gfxfb_fd >= 0) {
        close(gfxfb_fd);
        gfxfb_fd = -1;
    }

    return is_running;
}

bool is_presenting(void) {
    return is_running;
}

uint64_t presents_ended(void) {
    return __atomic_load_n(&ended, __ATOMIC_ACQUIRE);
}

uint64_t refresh_period_ns(void) {
    return period_ns;
}

bool stop_presenting(void) {
    __atomic_store_n(&is_stopping, true, __ATOMIC_RELEASE);
    pthread_join(presenter, nullptr);
    close(gfxfb_fd);
    is_running = false;

    printf(
        "gputest: %llu presents on output 0 ended, and %llu were not seen to latch within their deadline\n",
        static_cast<unsigned long long>(ended),
        static_cast<unsigned long long>(missed)
    );
    printf(
        "gputest: the longest present call took %llu us\n",
        static_cast<unsigned long long>(longest_call_ns / NS_PER_US)
    );

    return ended > 0 && missed == 0;
}
