// Checks /dev/gpu0 on virtio-gpu with venus: its facts, and buffers of its host-visible region, of system memory and
// of imported shared memory, mapped by the CPU and into contexts

#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gputest.h"
#include "virtio_gpu.h"

#define CAPSET_VENUS    4u // virtio-gpu's capset id of venus
#define ROOM_BUFFERS    4
#define ROOM_WAIT_NS    NS_PER_SEC
#define DESTROY_WAIT_US (100 * 1000)

// Past 1 MiB, so the backend allocates two pages for the import's page list, room for 512 entries
#define IMPORT_BYTES (2 * MIB)

// Venus's id for shared memory the host allocates outside Vulkan, which it creates only mappable
#define VENUS_SHARED_MEMORY_ID 0

// A new context ran no commands, so none of its venus memory has this id
#define UNALLOCATED_MEMORY_ID 0xD0D0u

static bool offers(const struct gpu_info* info, uint32_t placement) {
    return (info->placements & (1u << placement)) != 0;
}

// The arguments of a CONTEXT buffer of `context`'s venus memory `memory_id`
static struct gpu_buffer_args context_buffer(int32_t context, uint64_t size, uint32_t flags, uint64_t memory_id) {
    struct gpu_buffer_args args = {0};
    args.size = size;
    args.placement = GPU_PLACEMENT_CONTEXT;
    args.flags = flags;
    args.context = context;
    args.backend_id = memory_id;

    return args;
}

// A closed buffer's range returns only once the host answered its release on the driver's task, so creates retry
static int create_retrying(int fd, struct gpu_buffer_args* args) {
    uint64_t deadline = monotonic_ns() + ROOM_WAIT_NS;
    int error = gpu_call(fd, GPU_BUFFER_CREATE, args);
    while (error == ENOMEM && monotonic_ns() < deadline) {
        usleep(POLL_US);
        error = gpu_call(fd, GPU_BUFFER_CREATE, args);
    }

    return error;
}

// Without user addresses a context maps whole buffers and unmaps one by its device id
static int map_whole(int fd, int32_t context, int32_t buffer, uint32_t flags) {
    struct gpu_map_args args = {0};
    args.context = context;
    args.buffer = buffer;
    args.flags = flags;

    return gpu_call(fd, GPU_MAP, &args);
}

static int unmap_whole(int fd, int32_t context, uint32_t device_id) {
    struct gpu_map_args args = {0};
    args.context = context;
    args.va = device_id;

    return gpu_call(fd, GPU_UNMAP, &args);
}

static void print_facts(const struct gpu_info* info) {
    printf(
        "gputest: %s, capset mask 0x%llx, %llu MiB host-visible, system memory %s\n",
        info->name,
        (unsigned long long)info->virtio.capsets,
        (unsigned long long)(info->virtio.host_visible_size / MIB),
        offers(info, GPU_PLACEMENT_SYSTEM) ? "offered" : "not offered"
    );
}

static void test_facts(int fd, const struct gpu_info* info) {
    bool has_venus = (info->virtio.capsets & (1ULL << CAPSET_VENUS)) != 0;
    check("the GPU is virtio-gpu offering venus", info->backend == GPU_BACKEND_VIRTIO && has_venus);

    bool no_video = !offers(info, GPU_PLACEMENT_VIDEO) && info->video_size == 0 && info->mappable_video_size == 0;
    bool no_user_range = info->va_start == 0 && info->va_end == 0;
    check(
        "it offers context memory, without video memory or user addresses",
        offers(info, GPU_PLACEMENT_CONTEXT) && no_video && no_user_range
    );

    uint64_t region = info->virtio.host_visible_size;
    bool has_4k_pages = info->page_size == PAGE_BYTES;
    bool holds_whole_pages = region != 0 && region % PAGE_BYTES == 0;
    check("its pages are 4 KiB and its host-visible region holds whole pages", has_4k_pages && holds_whole_pages);

    struct gpu_timestamp timestamp = {0};
    struct gpu_memory memory = {0};
    bool no_clock = gpu_call(fd, GPU_READ_TIMESTAMP, &timestamp) == ENOTTY;
    bool reads_memory = gpu_call(fd, GPU_GET_MEMORY, &memory) == 0;
    bool none_free = reads_memory && memory.video_free == 0 && memory.mappable_video_free == 0;
    check("it has no clock and reports no free video memory", no_clock && none_free);
}

// The host keeps a blob past the destroy of the context that created it, so the buffer outlives that context
static void test_context_buffer(int fd) {
    int32_t first = create_context(fd);
    struct gpu_buffer_args args = context_buffer(first, BUFFER_BYTES, GPU_BUFFER_MAPPABLE, VENUS_SHARED_MEMORY_ID);
    int32_t buffer = gpu_call(fd, GPU_BUFFER_CREATE, &args) == 0 ? args.buffer : -1;

    volatile uint32_t* words = buffer >= 0 ? map_for_cpu(buffer) : NULL;
    if (words) {
        fill(words, FILL_WORD);
    }

    bool keeps_writes = words && reads_as(words, FILL_WORD);
    check("a mappable context buffer of venus's shared memory keeps what the CPU writes", keeps_writes);

    // The driver's task destroys the host's context after the close returns
    if (first >= 0) {
        close(first);
    }

    usleep(DESTROY_WAIT_US);

    int32_t second = words ? create_context(fd) : -1;
    bool refuses_read_only = second >= 0 && map_whole(fd, second, buffer, GPU_MAP_READ_ONLY) == EINVAL;
    check("a context refuses to map it read-only, since the host attaches with no access mode", refuses_read_only);

    bool maps = second >= 0 && map_whole(fd, second, buffer, 0) == 0;
    bool still_reads = maps && reads_as(words, FILL_WORD);
    check("a second context maps it once the first closed, and the CPU still reads what was written", still_reads);
    check("the second context unmaps it", maps && unmap_whole(fd, second, args.device_id) == 0);

    if (second >= 0) {
        close(second);
    }

    release(buffer, words);
}

static void test_context_refusals(int fd) {
    int32_t context = create_context(fd);
    struct gpu_buffer_args unmappable = context_buffer(context, BUFFER_BYTES, 0, VENUS_SHARED_MEMORY_ID);
    int unmappable_error = gpu_call(fd, GPU_BUFFER_CREATE, &unmappable);
    check("venus's shared memory is refused unless mappable", unmappable_error == ENOMEM);

    uint32_t flags = GPU_BUFFER_MAPPABLE;
    struct gpu_buffer_args unallocated = context_buffer(context, BUFFER_BYTES, flags, UNALLOCATED_MEMORY_ID);
    int unallocated_error = gpu_call(fd, GPU_BUFFER_CREATE, &unallocated);
    check("memory its context never allocated is refused", unallocated_error == ENOMEM);

    release(unmappable_error == 0 ? unmappable.buffer : -1, NULL);
    release(unallocated_error == 0 ? unallocated.buffer : -1, NULL);
    if (context >= 0) {
        close(context);
    }
}

// Two buffers mapped at once each reach only their own range, so neither reads what was written through the other
static void test_own_ranges(int fd) {
    int32_t context = create_context(fd);
    struct gpu_buffer_args first = context_buffer(context, BUFFER_BYTES, GPU_BUFFER_MAPPABLE, VENUS_SHARED_MEMORY_ID);
    struct gpu_buffer_args second = context_buffer(context, BUFFER_BYTES, GPU_BUFFER_MAPPABLE, VENUS_SHARED_MEMORY_ID);
    int32_t first_buffer = create_retrying(fd, &first) == 0 ? first.buffer : -1;
    int32_t second_buffer = create_retrying(fd, &second) == 0 ? second.buffer : -1;

    volatile uint32_t* first_words = first_buffer >= 0 ? map_for_cpu(first_buffer) : NULL;
    volatile uint32_t* second_words = second_buffer >= 0 ? map_for_cpu(second_buffer) : NULL;
    if (first_words && second_words) {
        fill(first_words, FILL_WORD);
        fill(second_words, ~FILL_WORD);
    }

    bool first_keeps = first_words && reads_as(first_words, FILL_WORD);
    bool second_keeps = second_words && reads_as(second_words, ~FILL_WORD);
    check("two mappable context buffers each read back their own word", first_keeps && second_keeps);

    release(first_buffer, first_words);
    release(second_buffer, second_words);
    if (context >= 0) {
        close(context);
    }
}

// Four buffers of a quarter each fill the region, so a fifth fits only once one of them returned its range
static void test_region_room(int fd, const struct gpu_info* info) {
    uint64_t quarter = info->virtio.host_visible_size / ROOM_BUFFERS / PAGE_BYTES * PAGE_BYTES;
    int32_t context = create_context(fd);

    // Waits for every earlier range to return, or the quarters start past one still held and the fourth does not fit
    struct gpu_buffer_args whole = context_buffer(
        context,
        ROOM_BUFFERS * quarter,
        GPU_BUFFER_MAPPABLE,
        VENUS_SHARED_MEMORY_ID
    );
    if (create_retrying(fd, &whole) == 0) {
        release(whole.buffer, NULL);
    }

    int32_t buffers[ROOM_BUFFERS];
    int created = 0;
    for (int i = 0; i < ROOM_BUFFERS; i++) {
        struct gpu_buffer_args args = context_buffer(context, quarter, GPU_BUFFER_MAPPABLE, VENUS_SHARED_MEMORY_ID);
        buffers[i] = create_retrying(fd, &args) == 0 ? args.buffer : -1;
        created += buffers[i] >= 0;
    }

    struct gpu_buffer_args fifth = context_buffer(context, quarter, GPU_BUFFER_MAPPABLE, VENUS_SHARED_MEMORY_ID);
    bool refused = gpu_call(fd, GPU_BUFFER_CREATE, &fifth) == ENOMEM;
    bool holds_four = created == ROOM_BUFFERS && refused;
    check("the host-visible region holds four buffers of a quarter of it and refuses a fifth", holds_four);

    // Not the first range: a backend mapping every blob at offset 0 would pass there
    int last = ROOM_BUFFERS - 1;
    volatile uint32_t* words = buffers[last] >= 0 ? map_for_cpu(buffers[last]) : NULL;
    if (words) {
        fill(words, FILL_WORD);
    }

    release(buffers[last], words);
    buffers[last] = create_retrying(fd, &fifth) == 0 ? fifth.buffer : -1;
    check("closing one returns its range to a fifth within a second", buffers[last] >= 0);

    words = buffers[last] >= 0 ? map_for_cpu(buffers[last]) : NULL;
    check("the fifth reads zero where the closed one was filled", words && reads_as(words, 0));

    release(buffers[last], words);
    for (int i = 0; i < last; i++) {
        release(buffers[i], NULL);
    }

    if (context >= 0) {
        close(context);
    }
}

static void test_system_buffer(int fd, const struct gpu_info* info) {
    struct gpu_buffer_args args = {0};
    args.size = BUFFER_BYTES;
    args.placement = GPU_PLACEMENT_SYSTEM;
    args.flags = GPU_BUFFER_MAPPABLE;

    int error = gpu_call(fd, GPU_BUFFER_CREATE, &args);
    int32_t buffer = error == 0 ? args.buffer : -1;
    if (!offers(info, GPU_PLACEMENT_SYSTEM)) {
        check("a system buffer is refused where system memory is not offered", error == EINVAL);
        release(buffer, NULL);
        return;
    }

    volatile uint32_t* words = buffer >= 0 ? map_for_cpu(buffer) : NULL;
    bool zeroed = words && reads_as(words, 0);
    if (words) {
        fill(words, FILL_WORD);
    }

    check("a system buffer reads zero and keeps what the CPU writes", zeroed && reads_as(words, FILL_WORD));

    int32_t context = buffer >= 0 ? create_context(fd) : -1;
    bool maps = context >= 0 && map_whole(fd, context, buffer, 0) == 0;
    check("a context maps and unmaps a system buffer", maps && unmap_whole(fd, context, args.device_id) == 0);

    if (context >= 0) {
        close(context);
    }

    release(buffer, words);
}

static void test_shared_memory_import(int fd, const struct gpu_info* info) {
    int shared = memfd_create("gputest", 0);
    struct gpu_buffer_args args = {0};
    args.source = shared;

    bool sized = shared >= 0 && ftruncate(shared, IMPORT_BYTES) == 0;
    int error = sized ? gpu_call(fd, GPU_BUFFER_IMPORT, &args) : -1;
    int32_t buffer = error == 0 ? args.buffer : -1;
    if (!offers(info, GPU_PLACEMENT_SYSTEM)) {
        check("shared memory import is refused where system memory is not offered", error == EINVAL);
    } else {
        bool is_system = buffer >= 0 && args.placement == GPU_PLACEMENT_SYSTEM && args.size == IMPORT_BYTES;
        check("2 MiB of shared memory imports as a system buffer of its size", is_system);

        int32_t context = buffer >= 0 ? create_context(fd) : -1;
        bool maps = context >= 0 && map_whole(fd, context, buffer, 0) == 0;
        check("a context maps and unmaps the import", maps && unmap_whole(fd, context, args.device_id) == 0);

        if (context >= 0) {
            close(context);
        }
    }

    release(buffer, NULL);
    if (shared >= 0) {
        close(shared);
    }
}

void test_virtio_gpu(int fd, const struct gpu_info* info) {
    print_facts(info);
    test_facts(fd, info);

    test_context_buffer(fd);
    test_context_refusals(fd);
    test_own_ranges(fd);
    test_region_room(fd, info);

    test_system_buffer(fd, info);
    test_shared_memory_import(fd, info);
}
