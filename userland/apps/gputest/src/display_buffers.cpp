// Creates and destroys gfxfb's buffers in stlxdm's place, from a thread of its own, while the remap check's contexts
// remap, fill and copy

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stlx/gfxfb_abi.h>

extern "C" {
#include "gputest.h"
}

static int       gfxfb_fd = -1;
static pthread_t cycling_thread;
static bool      is_stopping = false;
static uint32_t  width = 0;
static uint32_t  height = 0;
static uint64_t  cycles = 0;         // how many buffers were created and destroyed
static bool      has_failed = false; // whether a failed call ended the cycling early

static void record_failure(const char* call) {
    uint64_t cycled = cycled_buffers();
    printf(
        "gputest: %s failed after creating and destroying %llu %s: %s\n",
        call,
        static_cast<unsigned long long>(cycled),
        cycled == 1 ? "buffer" : "buffers",
        strerror(errno)
    );
    has_failed = true;
}

static void close_gfxfb() {
    close(gfxfb_fd);
    gfxfb_fd = -1;
}

static void* cycle_until_stopped(void*) {
    while (!__atomic_load_n(&is_stopping, __ATOMIC_ACQUIRE)) {
        gfxfb::gfxfb_buffer_args args = {};
        args.width = width;
        args.height = height;
        args.format = gfxfb::GFXFB_FORMAT_XRGB8888;
        if (ioctl(gfxfb_fd, gfxfb::GFXFB_CREATE_BUFFER, &args) != 0) {
            record_failure("GFXFB_CREATE_BUFFER");
            break;
        }

        if (ioctl(gfxfb_fd, gfxfb::GFXFB_DESTROY_BUFFER, static_cast<unsigned long>(args.handle)) != 0) {
            record_failure("GFXFB_DESTROY_BUFFER");
            break;
        }

        __atomic_fetch_add(&cycles, 1, __ATOMIC_RELAXED);
    }

    return nullptr;
}

bool start_cycling_buffers(void) {
    gfxfb_fd = open("/dev/gfxfb", O_RDWR);
    if (gfxfb_fd < 0) {
        printf("gputest: /dev/gfxfb cannot be opened: %s\n", strerror(errno));
        return false;
    }

    gfxfb::gfxfb_info primary = {};
    if (ioctl(gfxfb_fd, gfxfb::GFXFB_GET_INFO, &primary) != 0) {
        printf("gputest: GFXFB_GET_INFO failed: %s\n", strerror(errno));
        close_gfxfb();
        return false;
    }

    width = static_cast<uint32_t>(primary.width);
    height = static_cast<uint32_t>(primary.height);

    int error = pthread_create(&cycling_thread, nullptr, cycle_until_stopped, nullptr);
    if (error != 0) {
        printf("gputest: the thread cycling gfxfb's buffers cannot start: %s\n", strerror(error));
        close_gfxfb();
        return false;
    }

    return true;
}

uint64_t cycled_buffers(void) {
    return __atomic_load_n(&cycles, __ATOMIC_RELAXED);
}

bool stop_cycling_buffers(void) {
    __atomic_store_n(&is_stopping, true, __ATOMIC_RELEASE);
    pthread_join(cycling_thread, nullptr);
    close_gfxfb();

    return !has_failed;
}
