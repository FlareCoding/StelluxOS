// Checks /dev/gpu0 on virtio-gpu with venus: its facts, buffers of its host-visible region, of system memory and of
// imported shared memory, mapped by the CPU and into contexts, and venus command lists on its graphics queues

#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gputest.h"
#include "venus.h"
#include "virtio_gpu.h"

#define CAPSET_VENUS    4u // virtio-gpu's capset id of venus
#define ROOM_BUFFERS    4
#define ROOM_WAIT_NS    NS_PER_SEC
#define DESTROY_WAIT_US (100 * 1000)

#define SUBMIT_QUEUES    2
#define LISTS_PER_QUEUE  GPU_QUEUE_PENDING_MAX
#define REPLY_SLOT_BYTES 256
#define REPLY_WAIT_NS    NS_PER_SEC

// A reply's first word repeats its command's number, and a slot reading zero would pass for vkCreateInstance's
#define NO_REPLY_WORD 0xFFFFFFFFu

// Past 1 MiB, so the backend allocates two pages for the import's page list, room for 512 entries
#define IMPORT_BYTES (2 * MIB)

// Venus's id for shared memory the host allocates outside Vulkan, which it creates only mappable
#define VENUS_SHARED_MEMORY_ID 0

// A new context ran no commands, so none of its venus memory has this id
#define UNALLOCATED_MEMORY_ID 0xD0D0u

// Each list waits this long for a fence nothing signals, holding the host's decoder. Two waits outlast two frames,
// so a present comes while the flood's first chains are out and finds descriptors only if the chains left some
#define HOLD_NS (20 * NS_PER_SEC / 1000)

// Past GPU_SUBMIT_TIMEOUT_NS, so the core loses the list's context while its chain is out, and short of the stop
// deadline the backend then sets, GPU_SUBMIT_TIMEOUT_NS after that loss
#define HUNG_HOLD_NS (10 * NS_PER_SEC)

// The ids gputest gives its venus objects, unique within a context
#define INSTANCE_ID        1u
#define PHYSICAL_DEVICE_ID 2u
#define DEVICE_ID          3u
#define FENCE_ID           4u
#define MEMORY_ID          5u

// A context with two graphics queues, a timeline each, and a mappable buffer of venus's shared memory that holds each
// list's reply in a slot of its own
struct venus_session {
    int                fd;
    int32_t            context;
    int32_t            replies;
    uint32_t           replies_id; // the buffer's virtio resource id, where each list points the context's reply stream
    volatile uint32_t* reply_words;
    uint32_t           next_slot;

    uint32_t           queues[SUBMIT_QUEUES];
    int32_t            timelines[SUBMIT_QUEUES];
    uint64_t           points[SUBMIT_QUEUES]; // the value each queue's last submission signals
    uint32_t           queue_count;           // the queues opened, from the first

    int                memory_type; // venus's first host-visible and host-coherent memory type, or -1
};

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
    check("it offers graphics queues alone", info->queue_kinds == (1u << GPU_QUEUE_GRAPHICS));

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

// Opens the session's next queue with its timeline. Returns false, with neither left open, when a call failed
static bool open_queue(struct venus_session* s) {
    struct gpu_timeline_args timeline = {0};
    if (gpu_call(s->fd, GPU_TIMELINE_CREATE, &timeline) != 0) {
        return false;
    }

    struct gpu_queue_args queue = {0};
    queue.context = s->context;
    queue.kind = GPU_QUEUE_GRAPHICS;
    if (gpu_call(s->fd, GPU_QUEUE_CREATE, &queue) != 0) {
        close(timeline.timeline);
        return false;
    }

    s->queues[s->queue_count] = queue.queue;
    s->timelines[s->queue_count] = timeline.timeline;
    s->queue_count++;

    return true;
}

// Starts `list` by pointing the context's reply stream at the session's next slot, and returns that slot
static const volatile uint32_t* begin_reply(struct venus_session* s, struct venus_list* list) {
    uint64_t offset = (uint64_t)s->next_slot * REPLY_SLOT_BYTES;
    s->next_slot++;

    list->count = 0;
    venus_set_reply_stream(list, s->replies_id, offset, REPLY_SLOT_BYTES);

    return s->reply_words + offset / sizeof(uint32_t);
}

// Submits `list` on queue `q` to signal its next point, held until `wait` is reached when one is given. Returns 0 or
// the call's errno
static int submit_list(
    struct venus_session* s,
    uint32_t q,
    const struct venus_list* list,
    const struct gpu_point* wait
) {
    struct gpu_point signal = {0};
    signal.timeline = s->timelines[q];
    signal.value = s->points[q] + 1;

    struct gpu_submit_args args = {0};
    args.context = s->context;
    args.queue = s->queues[q];
    args.commands = (uint64_t)(uintptr_t)list->words;
    args.commands_size = list->count * sizeof(uint32_t);

    args.waits = (uint64_t)(uintptr_t)wait;
    args.wait_count = wait ? 1 : 0;
    args.signals = (uint64_t)(uintptr_t)&signal;
    args.signal_count = 1;

    int error = gpu_call(s->fd, GPU_SUBMIT, &args);
    if (error == 0) {
        s->points[q]++;
    }

    return error;
}

// Waits until every queue reached its last point, at most until `deadline_ns`. Returns 0 or the wait's errno
static int wait_for_queues(struct venus_session* s, uint64_t deadline_ns) {
    struct gpu_point lasts[SUBMIT_QUEUES] = {0};
    for (uint32_t i = 0; i < s->queue_count; i++) {
        lasts[i].timeline = s->timelines[i];
        lasts[i].value = s->points[i];
    }

    struct gpu_wait_args args = {0};
    args.points = (uint64_t)(uintptr_t)lasts;
    args.deadline_ns = deadline_ns;
    args.count = s->queue_count;
    args.flags = GPU_WAIT_ALL;

    return gpu_call(s->fd, GPU_TIMELINE_WAIT, &args);
}

// Whether queue `q`'s timeline carries GPU_TIMELINE_FAILED, true when the query fails
static bool has_failed_point(struct venus_session* s, uint32_t q) {
    struct gpu_timeline_args query = {0};
    query.timeline = s->timelines[q];
    return gpu_call(s->fd, GPU_TIMELINE_QUERY, &query) != 0 || (query.flags & GPU_TIMELINE_FAILED) != 0;
}

// Runs `list` on the first queue. Returns whether it ended unfailed within REPLY_WAIT_NS
static bool run_list(struct venus_session* s, const struct venus_list* list) {
    bool submitted = submit_list(s, 0, list, NULL) == 0;
    return submitted && wait_for_queues(s, monotonic_ns() + REPLY_WAIT_NS) == 0 && !has_failed_point(s, 0);
}

// Runs `list`, then reads its reply of `command` in `reply`. Returns whether venus answered VK_SUCCESS
static bool runs_to_success(
    struct venus_session* s,
    const struct venus_list* list,
    const volatile uint32_t* reply,
    uint32_t command
) {
    int32_t result = -1;
    return run_list(s, list) && venus_result(reply, command, &result) && result == VENUS_SUCCESS;
}

// Sets up venus from instance to fence, finding the memory type venus allocates from. Returns whether each step but
// the memory query answered VK_SUCCESS and venus offers a host-visible and host-coherent type
static bool set_up_venus(struct venus_session* s) {
    struct venus_list list;
    const volatile uint32_t* reply = begin_reply(s, &list);
    venus_create_instance(&list, INSTANCE_ID);
    bool has_instance = runs_to_success(s, &list, reply, VENUS_CREATE_INSTANCE);

    reply = begin_reply(s, &list);
    venus_enumerate_physical_devices(&list, INSTANCE_ID, PHYSICAL_DEVICE_ID);
    bool has_physical_device = has_instance && runs_to_success(s, &list, reply, VENUS_ENUMERATE_PHYSICAL_DEVICES);

    reply = begin_reply(s, &list);
    venus_get_memory_properties(&list, PHYSICAL_DEVICE_ID);
    bool has_properties = has_physical_device && run_list(s, &list);
    s->memory_type = has_properties ? venus_host_visible_type(reply) : -1;

    reply = begin_reply(s, &list);
    venus_create_device(&list, PHYSICAL_DEVICE_ID, DEVICE_ID);
    bool has_device = s->memory_type >= 0 && runs_to_success(s, &list, reply, VENUS_CREATE_DEVICE);

    reply = begin_reply(s, &list);
    venus_create_fence(&list, DEVICE_ID, FENCE_ID);
    bool has_fence = has_device && runs_to_success(s, &list, reply, VENUS_CREATE_FENCE);

    return has_fence;
}

// Opens the session's context, reply buffer and queues, then sets up venus. Returns false when a step failed, leaving
// what it made for close_session()
static bool open_session(int fd, struct venus_session* s) {
    s->fd = fd;
    s->context = create_context(fd);
    s->replies = -1;
    s->memory_type = -1;

    struct gpu_buffer_args replies = context_buffer(
        s->context,
        BUFFER_BYTES,
        GPU_BUFFER_MAPPABLE,
        VENUS_SHARED_MEMORY_ID
    );
    if (s->context < 0 || create_retrying(fd, &replies) != 0) {
        return false;
    }

    s->replies = replies.buffer;
    s->replies_id = replies.device_id;
    s->reply_words = map_for_cpu(s->replies);
    if (!s->reply_words || map_whole(fd, s->context, s->replies, 0) != 0) {
        return false;
    }

    fill(s->reply_words, NO_REPLY_WORD);
    for (int i = 0; i < SUBMIT_QUEUES; i++) {
        if (!open_queue(s)) {
            return false;
        }
    }

    return set_up_venus(s);
}

static void close_session(struct venus_session* s) {
    for (uint32_t i = 0; i < s->queue_count; i++) {
        close(s->timelines[i]);
    }

    if (s->context >= 0) {
        close(s->context);
    }

    release(s->replies, s->reply_words);
}

// Allocates BUFFER_BYTES of venus's host-visible memory type as memory MEMORY_ID. Returns whether venus answered
// VK_SUCCESS
static bool allocate_memory(struct venus_session* s) {
    struct venus_list list;
    const volatile uint32_t* reply = begin_reply(s, &list);
    venus_allocate_memory(&list, DEVICE_ID, MEMORY_ID, BUFFER_BYTES, (uint32_t)s->memory_type);

    return runs_to_success(s, &list, reply, VENUS_ALLOCATE_MEMORY);
}

static void test_allocated_memory(int fd) {
    struct venus_session s = {0};
    bool opened = open_session(fd, &s);
    check("venus answers its setup from instance to fence and offers host-visible, host-coherent memory", opened);

    struct gpu_buffer_args args = context_buffer(s.context, BUFFER_BYTES, GPU_BUFFER_MAPPABLE, MEMORY_ID);
    bool allocated = opened && allocate_memory(&s);
    int32_t buffer = allocated && create_retrying(fd, &args) == 0 ? args.buffer : -1;

    volatile uint32_t* words = buffer >= 0 ? map_for_cpu(buffer) : NULL;
    if (words) {
        fill(words, FILL_WORD);
    }

    bool keeps_writes = words && reads_as(words, FILL_WORD);
    check("1 MiB from vkAllocateMemory maps as a mappable context buffer and keeps what the CPU writes", keeps_writes);

    release(buffer, words);
    close_session(&s);
}

static void test_freed_memory(int fd) {
    struct venus_session s = {0};
    bool allocated = open_session(fd, &s) && allocate_memory(&s);

    struct gpu_buffer_args args = context_buffer(s.context, BUFFER_BYTES, GPU_BUFFER_MAPPABLE, MEMORY_ID);
    bool created = allocated && create_retrying(fd, &args) == 0;
    release(created ? args.buffer : -1, NULL);

    // The driver's task gives the resource back after the close returns, and vkFreeMemory follows that
    usleep(DESTROY_WAIT_US);

    struct venus_list list = {0};
    venus_free_memory(&list, DEVICE_ID, MEMORY_ID);
    bool freed = created && run_list(&s, &list);

    struct gpu_buffer_args again = context_buffer(s.context, BUFFER_BYTES, GPU_BUFFER_MAPPABLE, MEMORY_ID);
    int error = freed ? gpu_call(fd, GPU_BUFFER_CREATE, &again) : -1;
    check("once its buffer is released and vkFreeMemory ran, the memory is refused for a buffer", error == ENOMEM);

    release(error == 0 ? again.buffer : -1, NULL);
    close_session(&s);
}

// Each queue's first list waits for a point nothing reached, so the queue holds all 64. Once the point is reached the
// 128 lists, each waiting HOLD_NS on the host, outnumber the chains a full batch leaves descriptors for
static void test_list_flood(int fd) {
    struct venus_session s = {0};
    struct gpu_timeline_args gate = {0};
    bool has_gate = open_session(fd, &s) && gpu_call(fd, GPU_TIMELINE_CREATE, &gate) == 0;
    struct gpu_point opening = {0};
    opening.timeline = gate.timeline;
    opening.value = 1;

    const volatile uint32_t* replies[SUBMIT_QUEUES * LISTS_PER_QUEUE];
    uint32_t submitted = 0;
    struct venus_list list;
    for (uint32_t q = 0; q < SUBMIT_QUEUES && has_gate; q++) {
        for (uint32_t i = 0; i < LISTS_PER_QUEUE; i++) {
            const volatile uint32_t* reply = begin_reply(&s, &list);
            venus_wait_for_fence(&list, DEVICE_ID, FENCE_ID, HOLD_NS);
            if (submit_list(&s, q, &list, i == 0 ? &opening : NULL) == 0) {
                replies[submitted] = reply;
                submitted++;
            }
        }
    }

    bool holds_all = submitted == SUBMIT_QUEUES * LISTS_PER_QUEUE;
    bool is_full = holds_all;
    for (uint32_t q = 0; q < SUBMIT_QUEUES && is_full; q++) {
        is_full = submit_list(&s, q, &list, NULL) == EBUSY;
    }

    check(
        "each graphics queue holds 64 lists, the first waiting for a point nothing reached, and refuses a 65th",
        is_full
    );

    struct gpu_timeline_args signal = {0};
    signal.timeline = gate.timeline;
    signal.value = opening.value;

    uint64_t presents_before = presents_ended();
    uint64_t start_ns = monotonic_ns();
    bool is_signaled = has_gate && gpu_call(fd, GPU_TIMELINE_SIGNAL, &signal) == 0;
    bool ended = is_signaled && wait_for_queues(&s, start_ns + GPU_SUBMIT_TIMEOUT_NS) == 0;
    uint64_t presents_during = presents_ended() - presents_before;

    unsigned long long elapsed_ms = (monotonic_ns() - start_ns) / (NS_PER_SEC / 1000);
    if (ended) {
        printf(
            "gputest: %u venus lists ended %llu ms after the point their queues waited for\n",
            submitted,
            elapsed_ms
        );
    }

    bool all_timed_out = holds_all && ended && !has_failed_point(&s, 0) && !has_failed_point(&s, 1);
    for (uint32_t i = 0; i < submitted && all_timed_out; i++) {
        int32_t result = -1;
        all_timed_out = venus_result(replies[i], VENUS_WAIT_FOR_FENCES, &result) && result == VENUS_TIMEOUT;
    }

    check("128 lists across two queues each wait 20 ms on the host and all end with VK_TIMEOUT", all_timed_out);
    if (is_presenting()) {
        check("presents on output 0 ended while the lists ran", presents_during > 0);
    }

    if (has_gate) {
        close(gate.timeline);
    }

    close_session(&s);
}

// A list of 0 bytes, which the backend sends as its header alone, still ends in order after the list before it
static void test_empty_list(int fd) {
    struct venus_session s = {0};
    bool opened = open_session(fd, &s);

    struct venus_list list = {0};
    const volatile uint32_t* reply = opened ? begin_reply(&s, &list) : NULL;
    venus_wait_for_fence(&list, DEVICE_ID, FENCE_ID, HOLD_NS);

    struct venus_list empty = {0};
    bool submitted = opened && submit_list(&s, 0, &list, NULL) == 0 && submit_list(&s, 0, &empty, NULL) == 0;
    bool ended = submitted && wait_for_queues(&s, monotonic_ns() + REPLY_WAIT_NS) == 0 && !has_failed_point(&s, 0);

    int32_t result = -1;
    bool follows = ended && venus_result(reply, VENUS_WAIT_FOR_FENCES, &result) && result == VENUS_TIMEOUT;
    check("an empty list ends after the list before it, which waited 20 ms on the host", follows);

    close_session(&s);
}

// The core loses the list's context at GPU_SUBMIT_TIMEOUT_NS and stops its queues, and the backend reports the stop
// only once the list's chain returns, so the list still ends with the host's answer
static void test_hung_list(int fd) {
    struct venus_session s = {0};
    bool opened = open_session(fd, &s);

    struct venus_list list = {0};
    const volatile uint32_t* reply = opened ? begin_reply(&s, &list) : NULL;
    venus_wait_for_fence(&list, DEVICE_ID, FENCE_ID, HUNG_HOLD_NS);

    uint64_t start_ns = monotonic_ns();
    bool submitted = opened && submit_list(&s, 0, &list, NULL) == 0;
    bool ended = submitted && wait_for_queues(&s, start_ns + HUNG_HOLD_NS + GPU_SUBMIT_TIMEOUT_NS) == 0;
    unsigned long long elapsed_ms = (monotonic_ns() - start_ns) / (NS_PER_SEC / 1000);
    if (ended) {
        printf("gputest: a venus list waiting 10 s on the host ended %llu ms after its submit\n", elapsed_ms);
    }

    int32_t result = -1;
    bool is_answered = ended && venus_result(reply, VENUS_WAIT_FOR_FENCES, &result) && result == VENUS_TIMEOUT;
    check(
        "a list waiting 10 s on the host, past the core's 7 s deadline, ends unfailed once the host answers it",
        is_answered && !has_failed_point(&s, 0)
    );

    struct gpu_context_args query = {0};
    query.context = s.context;
    bool is_hung = opened && gpu_call(fd, GPU_CONTEXT_QUERY, &query) == 0 && query.lost == GPU_LOST_HUNG;
    check("that list's context is lost as hung", is_hung);

    close_session(&s);

    struct venus_session fresh = {0};
    check("a fresh context then sets venus up, so the device did not fail", open_session(fd, &fresh));
    close_session(&fresh);
}

// Each closed session's reply buffer returns its range only once the session's queues were reported stopped
static void test_room_after_sessions(int fd, const struct gpu_info* info) {
    int32_t context = create_context(fd);
    uint64_t whole_bytes = info->virtio.host_visible_size;
    struct gpu_buffer_args whole = context_buffer(context, whole_bytes, GPU_BUFFER_MAPPABLE, VENUS_SHARED_MEMORY_ID);
    bool returned = context >= 0 && create_retrying(fd, &whole) == 0;
    check("a buffer of the whole host-visible region fits once every earlier venus context closed", returned);

    release(returned ? whole.buffer : -1, NULL);
    if (context >= 0) {
        close(context);
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

    test_allocated_memory(fd);
    test_freed_memory(fd);
    test_list_flood(fd);
    test_empty_list(fd);
    test_hung_list(fd);
    test_room_after_sessions(fd, info);
}
