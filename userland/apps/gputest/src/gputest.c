// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under
// kernel/drivers/graphics/nvidia/LICENSE-NVIDIA

// Checks /dev/gpu0 on an NVIDIA card, then the window protocol, or with `proto` only the protocol checks, which need no
// GPU. Prints a line per check and exits nonzero when any failed

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <stlx/gpu_abi.h>

#include "gputest.h"

#define NS_PER_SEC      1000000000ULL
#define NS_PER_MS       1000000ULL
#define MIB             (1024ULL * 1024ULL)
#define PAGE_BYTES      4096ULL
#define PAGE_WORDS      (PAGE_BYTES / sizeof(uint32_t))
#define USER_VA_START   PAGE_BYTES
#define USER_VA_END     (1ULL << 39)
#define BUFFER_BYTES    MIB
#define BUFFER_WORDS    (BUFFER_BYTES / sizeof(uint32_t))
#define CLOCK_SLEEP_US  (100 * 1000)
#define RELEASE_WAIT_NS (2 * NS_PER_SEC)
#define POLL_US         1000
#define CONTEXT_COUNT   16
#define FILL_WORD       0xA5C3E10Fu
#define QUEUE_COUNT     2
#define LIST_WORDS_MAX  32
#define WAIT_NS         (100 * NS_PER_MS)
#define WAITED_COPIES   1000000u
#define HOLD_US         (20 * 1000)
#define LOST_WAIT_NS    (GPU_SUBMIT_TIMEOUT_NS + 2 * NS_PER_SEC)

// The copy part's buffers in its context: each queue's command words in a page of the first, with the word a
// semaphore acquire reads in the page after them, then the memory they fill and copy
#define COMMANDS_VA  USER_VA_START
#define SEMAPHORE_VA (COMMANDS_VA + QUEUE_COUNT * PAGE_BYTES)
#define VIDEO_VA     (COMMANDS_VA + BUFFER_BYTES)
#define SYSTEM_VA    (VIDEO_VA + BUFFER_BYTES)
#define SHARED_VA    (SYSTEM_VA + BUFFER_BYTES)

// NVIDIA's NVC56F_DMA_SEC_OP_INC_METHOD, a push buffer header whose values go to consecutive methods, with the value
// count from bit 16, here on subchannel 0
#define PUSH_HEADER_INCREASING  (1u << 29)
#define PUSH_HEADER_COUNT_SHIFT 16

// NVIDIA's NVC56F host methods: SET_OBJECT (which binds the copy class to the subchannel), SEMAPHOREA to SEMAPHORED,
// MEM_OP_A to MEM_OP_D (which run MEM_OP_D's operation) and SET_REFERENCE
#define SET_OBJECT    0x0000u
#define SEMAPHORE_A   0x0010u
#define MEM_OP_A      0x0028u
#define SET_REFERENCE 0x0050u

// NVIDIA's SEMAPHORED fields: OPERATION ACQUIRE, which waits for the word to equal the payload, and ACQUIRE_SWITCH
// ENABLED, which lets other channels run meanwhile
#define SEMAPHORE_ACQUIRE (0x1u | (1u << 12))

// NVIDIA's MEM_OP_D operations in bits 31:27, the privileged TLB invalidate among them, and MEM_OP_C's
// TLB_INVALIDATE_PDB ALL. MEMBAR takes MEM_OP_C's SYS_MEMBAR type, 0
#define MEM_OP_MEMBAR               (0x05u << 27)
#define MEM_OP_TLB_INVALIDATE       (0x09u << 27)
#define MEM_OP_L2_SYSMEM_INVALIDATE (0x0Eu << 27)
#define TLB_INVALIDATE_ALL_PDBS     0x1u

// NVIDIA's NVC7B5 copy methods
#define SET_SRC_PHYS_MODE    0x0260u
#define LAUNCH_DMA           0x0300u
#define OFFSET_IN_UPPER      0x0400u
#define OFFSET_OUT_UPPER     0x0408u
#define LINE_LENGTH_IN       0x0418u
#define SET_REMAP_CONST_A    0x0700u
#define SET_REMAP_COMPONENTS 0x0708u

// NVIDIA's SET_REMAP_COMPONENTS fields: DST_X CONST_A, COMPONENT_SIZE FOUR, one destination component
#define REMAP_FILL_ELEMENTS (0x4u | (0x3u << 16))

// NVIDIA's LAUNCH_DMA fields: NON_PIPELINED, SRC and DST_MEMORY_LAYOUT PITCH and DISABLE_PLC, between virtual
// addresses. FLUSH_ENABLE stays clear, since the release ending each submission waits for idle and makes writes visible
#define LAUNCH_COPY (0x2u | (1u << 7) | (1u << 8) | (1u << 26))
#define LAUNCH_FILL (LAUNCH_COPY | (1u << 10)) // and REMAP_ENABLE, writing the remap's constant for each source word

// NVIDIA's SET_SRC_PHYS_MODE TARGET LOCAL_FB, a source in video memory, and LAUNCH_DMA's SRC_TYPE PHYSICAL, which
// reads the source there by its physical address
#define PHYS_MODE_VIDEO_MEMORY 0x0u
#define LAUNCH_FROM_PHYSICAL   (LAUNCH_COPY | (1u << 12))

// Method words for the copy engine, which a submission copies to its queue's command words
struct command_list {
    uint32_t count;
    uint32_t words[LIST_WORDS_MAX];
};

// A copy queue with its timeline and its command words, a page of the session's command buffer
struct copy_queue {
    uint32_t           index;
    int32_t            timeline;
    uint64_t           point; // the value its last submission signals
    uint64_t           words_va;
    volatile uint32_t* words;
};

// A context with copy queues and three buffers mapped for the CPU and the GPU: the queues' command words, read-only
// to the GPU, then the video and system memory they fill and copy, each check starting from what the last one left
struct copy_session {
    int                fd;
    uint32_t           copy_class;
    int32_t            context;
    struct copy_queue  queues[QUEUE_COUNT];
    uint32_t           queue_count; // the queues opened, from the first

    int32_t            commands;
    int32_t            video;
    int32_t            system;
    volatile uint32_t* command_words;
    volatile uint32_t* video_words;
    volatile uint32_t* system_words;
};

static int passed = 0;
static int failed = 0;

void check(const char* name, bool cond) {
    if (cond) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

// Returns 0 or the call's errno
static int gpu_call(int fd, uint32_t cmd, void* arg) {
    return ioctl(fd, cmd, arg) == 0 ? 0 : errno;
}

static struct gpu_memory read_memory(int fd) {
    struct gpu_memory memory = {0};
    gpu_call(fd, GPU_GET_MEMORY, &memory);
    return memory;
}

static bool same_memory(struct gpu_memory a, struct gpu_memory b) {
    return a.video_free == b.video_free && a.mappable_video_free == b.mappable_video_free;
}

// The driver's task releases memory after the last handle and mapping are gone, so it returns a little later
static bool memory_returns_to(int fd, struct gpu_memory expected) {
    uint64_t deadline = monotonic_ns() + RELEASE_WAIT_NS;
    while (!same_memory(read_memory(fd), expected)) {
        if (monotonic_ns() >= deadline) {
            return false;
        }

        usleep(POLL_US);
    }

    return true;
}

// Returns 0 with the new handle in `out`, or the call's errno with -1 in `out`
static int create_buffer(int fd, uint64_t size, uint32_t placement, uint32_t flags, int32_t* out) {
    struct gpu_buffer_args args = {0};
    args.size = size;
    args.placement = placement;
    args.flags = flags;

    int error = gpu_call(fd, GPU_BUFFER_CREATE, &args);
    *out = error == 0 ? args.buffer : -1;

    return error;
}

static volatile uint32_t* map_for_cpu(int32_t buffer) {
    void* words = mmap(NULL, BUFFER_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, buffer, 0);
    return words == MAP_FAILED ? NULL : words;
}

static bool reads_as(const volatile uint32_t* words, uint32_t value) {
    for (size_t i = 0; i < BUFFER_WORDS; i++) {
        if (words[i] != value) {
            return false;
        }
    }

    return true;
}

// On x86_64 the fence drains the CPU's write-combining buffers, so a later read sees memory
static void fill(volatile uint32_t* words, uint32_t value) {
    for (size_t i = 0; i < BUFFER_WORDS; i++) {
        words[i] = value;
    }

    __sync_synchronize();
}

static void write_indices(volatile uint32_t* words) {
    for (size_t i = 0; i < BUFFER_WORDS; i++) {
        words[i] = (uint32_t)i;
    }
}

static bool holds_indices(const volatile uint32_t* words) {
    for (size_t i = 0; i < BUFFER_WORDS; i++) {
        if (words[i] != (uint32_t)i) {
            return false;
        }
    }

    return true;
}

static void release(int32_t buffer, volatile uint32_t* words) {
    if (words) {
        munmap((void*)words, BUFFER_BYTES);
    }

    if (buffer >= 0) {
        close(buffer);
    }
}

static int32_t create_context(int fd) {
    struct gpu_context_args args = {0};
    return gpu_call(fd, GPU_CONTEXT_CREATE, &args) == 0 ? args.context : -1;
}

static int map(int fd, int32_t context, int32_t buffer, uint64_t va, uint32_t flags) {
    struct gpu_map_args args = {0};
    args.context = context;
    args.buffer = buffer;
    args.va = va;
    args.size = BUFFER_BYTES;
    args.flags = flags;

    return gpu_call(fd, GPU_MAP, &args);
}

static int unmap(int fd, int32_t context, uint64_t va) {
    struct gpu_map_args args = {0};
    args.context = context;
    args.va = va;
    args.size = BUFFER_BYTES;

    return gpu_call(fd, GPU_UNMAP, &args);
}

static void print_facts(const struct gpu_info* info) {
    printf(
        "gputest: %s, chipset 0x%x, %llu MiB of video memory with %llu MiB mappable, %u GPCs and %u TPCs\n",
        info->name,
        info->nvidia.chipset,
        (unsigned long long)(info->video_size / MIB),
        (unsigned long long)(info->mappable_video_size / MIB),
        info->nvidia.gpc_count,
        info->nvidia.tpc_count
    );
}

static void test_facts(const struct gpu_info* info) {
    bool is_nvidia = info->backend == GPU_BACKEND_NVIDIA;
    bool copy_queues_alone = info->queue_kinds == (1u << GPU_QUEUE_COPY);
    check("the GPU is NVIDIA's and offers copy queues alone", is_nvidia && copy_queues_alone);

    uint32_t video_and_system = (1u << GPU_PLACEMENT_VIDEO) | (1u << GPU_PLACEMENT_SYSTEM);
    bool user_range = info->va_start == USER_VA_START && info->va_end == USER_VA_END;
    bool small_pages = info->page_size == PAGE_BYTES;
    check(
        "it offers video and system memory and maps user addresses from 4 KiB to 2^39 in 4 KiB pages",
        info->placements == video_and_system && user_range && small_pages
    );

    bool named = info->name[0] != '\0';
    bool has_classes = info->nvidia.class_gpfifo != 0 && info->nvidia.class_copy != 0;
    bool counts_shaders = info->nvidia.gpc_count != 0 && info->nvidia.tpc_count >= info->nvidia.gpc_count;
    check(
        "it names its product and its channel and copy classes, and counts its shader units",
        named && has_classes && counts_shaders
    );

    bool has_window = info->mappable_video_size != 0 && info->mappable_video_size <= info->video_size;
    check("it has video memory with a CPU window into it", has_window);
}

static void test_free_memory(const struct gpu_info* info, struct gpu_memory memory) {
    bool video_within = memory.video_free != 0 && memory.video_free <= info->video_size;
    bool window_within = memory.mappable_video_free != 0 && memory.mappable_video_free <= info->mappable_video_size;
    check("free memory lies within video memory and the CPU's window", video_within && window_within);
}

static void test_clock(int fd) {
    struct gpu_timestamp before = {0};
    struct gpu_timestamp after = {0};
    bool read_before = gpu_call(fd, GPU_READ_TIMESTAMP, &before) == 0;
    usleep(CLOCK_SLEEP_US);
    bool read_after = gpu_call(fd, GPU_READ_TIMESTAMP, &after) == 0;

    uint64_t cpu_elapsed = after.cpu_ns - before.cpu_ns;
    uint64_t gpu_elapsed = after.gpu_ns - before.gpu_ns;
    uint64_t tolerance = cpu_elapsed / 10;
    bool rises = after.gpu_ns > before.gpu_ns;
    bool same_rate = gpu_elapsed + tolerance >= cpu_elapsed && gpu_elapsed <= cpu_elapsed + tolerance;
    check("the GPU's clock runs at the CPU clock's rate", read_before && read_after && rises && same_rate);
}

static void test_video_buffer(int fd) {
    struct gpu_memory before = read_memory(fd);
    int32_t buffer = -1;
    int error = create_buffer(fd, BUFFER_BYTES, GPU_PLACEMENT_VIDEO, GPU_BUFFER_MAPPABLE, &buffer);
    struct gpu_memory during = read_memory(fd);

    bool takes_video = before.video_free - during.video_free == BUFFER_BYTES;
    bool takes_window = before.mappable_video_free - during.mappable_video_free == BUFFER_BYTES;
    check(
        "a mappable video buffer takes its size from video memory and from the CPU's window",
        error == 0 && takes_video && takes_window
    );

    volatile uint32_t* words = error == 0 ? map_for_cpu(buffer) : NULL;
    check("a mappable video buffer reads zero through the CPU", words && reads_as(words, 0));

    if (words) {
        fill(words, FILL_WORD);
    }

    check("a mappable video buffer keeps what the CPU writes", words && reads_as(words, FILL_WORD));

    release(buffer, words);
    check("closing a video buffer gives its memory back", memory_returns_to(fd, before));

    // Video memory goes to the lowest free offset that fits, so this buffer lies where the filled one did
    error = create_buffer(fd, BUFFER_BYTES, GPU_PLACEMENT_VIDEO, GPU_BUFFER_MAPPABLE, &buffer);
    words = error == 0 ? map_for_cpu(buffer) : NULL;
    check("a video buffer made from the memory a filled one gave back reads zero", words && reads_as(words, 0));

    release(buffer, words);
    memory_returns_to(fd, before);
}

static void test_system_buffer(int fd) {
    struct gpu_memory before = read_memory(fd);
    int32_t buffer = -1;
    int error = create_buffer(fd, BUFFER_BYTES, GPU_PLACEMENT_SYSTEM, GPU_BUFFER_MAPPABLE, &buffer);
    check("a system buffer takes no video memory", error == 0 && same_memory(read_memory(fd), before));

    volatile uint32_t* words = error == 0 ? map_for_cpu(buffer) : NULL;
    bool zeroed = words && reads_as(words, 0);
    if (words) {
        fill(words, FILL_WORD);
    }

    check("a system buffer reads zero and keeps what the CPU writes", zeroed && reads_as(words, FILL_WORD));

    release(buffer, words);
}

static void test_scanout_buffer(int fd) {
    struct gpu_memory before = read_memory(fd);
    int32_t buffer = -1;
    int error = create_buffer(fd, BUFFER_BYTES, GPU_PLACEMENT_VIDEO, GPU_BUFFER_SCANOUT, &buffer);
    struct gpu_memory during = read_memory(fd);

    bool takes_video = before.video_free - during.video_free == BUFFER_BYTES;
    bool leaves_window = during.mappable_video_free == before.mappable_video_free;
    check(
        "a scanout buffer takes video memory and leaves the CPU's window",
        error == 0 && takes_video && leaves_window
    );

    release(buffer, NULL);
    memory_returns_to(fd, before);
}

static void test_window_refusal(int fd) {
    struct gpu_memory before = read_memory(fd);
    uint64_t size = before.mappable_video_free + BUFFER_BYTES;
    int32_t buffer = -1;
    int error = create_buffer(fd, size, GPU_PLACEMENT_VIDEO, GPU_BUFFER_MAPPABLE, &buffer);

    check(
        "a mappable buffer larger than the CPU's window has free is refused and gives its video memory back",
        error == ENOMEM && memory_returns_to(fd, before)
    );
    release(buffer, NULL);
}

static void test_context_mappings(int fd) {
    struct gpu_memory before = read_memory(fd);
    int32_t video = -1;
    int32_t system = -1;
    create_buffer(fd, BUFFER_BYTES, GPU_PLACEMENT_VIDEO, 0, &video);
    create_buffer(fd, BUFFER_BYTES, GPU_PLACEMENT_SYSTEM, 0, &system);

    struct gpu_context_args query = {0};
    query.context = create_context(fd);
    bool queried = query.context >= 0 && gpu_call(fd, GPU_CONTEXT_QUERY, &query) == 0;
    check("a new context is not lost", queried && query.lost == 0);

    uint64_t top = USER_VA_END - BUFFER_BYTES;
    bool maps_video = map(fd, query.context, video, USER_VA_START, 0) == 0;
    bool maps_system = map(fd, query.context, system, USER_VA_START + BUFFER_BYTES, 0) == 0;
    bool maps_top_read_only = map(fd, query.context, video, top, GPU_MAP_READ_ONLY) == 0;
    check(
        "a context maps video and system memory, and read-only at the top of its range",
        maps_video && maps_system && maps_top_read_only
    );

    bool unmaps_video = unmap(fd, query.context, USER_VA_START) == 0;
    bool unmaps_system = unmap(fd, query.context, USER_VA_START + BUFFER_BYTES) == 0;
    bool unmaps_top = unmap(fd, query.context, top) == 0;
    check("a context unmaps each of its mappings", unmaps_video && unmaps_system && unmaps_top);

    close(query.context);
    release(video, NULL);
    release(system, NULL);
    check("closing a context and its buffers gives their memory back", memory_returns_to(fd, before));
}

static void test_shared_memory_import(int fd) {
    struct gpu_memory before = read_memory(fd);
    int shared = memfd_create("gputest", 0);
    struct gpu_buffer_args args = {0};
    args.source = shared;

    bool sized = shared >= 0 && ftruncate(shared, BUFFER_BYTES) == 0;
    bool imported = sized && gpu_call(fd, GPU_BUFFER_IMPORT, &args) == 0;
    bool is_system = imported && args.placement == GPU_PLACEMENT_SYSTEM && args.size == BUFFER_BYTES;
    check("shared memory imports as a system buffer of its size", is_system);

    int32_t context = imported ? create_context(fd) : -1;
    bool maps = context >= 0 && map(fd, context, args.buffer, USER_VA_START, 0) == 0;
    bool unmaps = maps && unmap(fd, context, USER_VA_START) == 0;
    check("a context maps and unmaps imported shared memory", maps && unmaps);

    if (context >= 0) {
        close(context);
    }

    release(imported ? args.buffer : -1, NULL);
    if (shared >= 0) {
        close(shared);
    }

    memory_returns_to(fd, before);
}

static void test_many_contexts(int fd) {
    struct gpu_memory before = read_memory(fd);
    int32_t buffer = -1;
    create_buffer(fd, BUFFER_BYTES, GPU_PLACEMENT_VIDEO, 0, &buffer);

    int32_t contexts[CONTEXT_COUNT];
    int mapped = 0;
    for (int i = 0; i < CONTEXT_COUNT; i++) {
        contexts[i] = create_context(fd);
        mapped += contexts[i] >= 0 && map(fd, contexts[i], buffer, USER_VA_START, 0) == 0;
    }

    check("sixteen contexts at once each map one buffer", mapped == CONTEXT_COUNT);

    for (int i = 0; i < CONTEXT_COUNT; i++) {
        if (contexts[i] >= 0) {
            close(contexts[i]);
        }
    }

    release(buffer, NULL);
    check("closing them and the buffer gives their memory back", memory_returns_to(fd, before));
}

// A push buffer header sending `count` values to consecutive methods from `method`
static uint32_t method_header(uint32_t method, uint32_t count) {
    return PUSH_HEADER_INCREASING | (count << PUSH_HEADER_COUNT_SHIFT) | (method >> 2);
}

static void push_method(struct command_list* list, uint32_t method, uint32_t value) {
    list->words[list->count++] = method_header(method, 1);
    list->words[list->count++] = value;
}

// Sends the upper word of `va` to `upper_method` and its lower word to the method after it
static void push_address(struct command_list* list, uint32_t upper_method, uint64_t va) {
    list->words[list->count++] = method_header(upper_method, 2);
    list->words[list->count++] = (uint32_t)(va >> 32);
    list->words[list->count++] = (uint32_t)va;
}

// Has the copy engine write `value` to every word of the buffer at `va`
static void push_fill(struct command_list* list, uint32_t copy_class, uint64_t va, uint32_t value) {
    push_method(list, SET_OBJECT, copy_class);
    push_method(list, SET_REMAP_CONST_A, value);
    push_method(list, SET_REMAP_COMPONENTS, REMAP_FILL_ELEMENTS);

    push_address(list, OFFSET_OUT_UPPER, va);
    push_method(list, LINE_LENGTH_IN, (uint32_t)BUFFER_WORDS);
    push_method(list, LAUNCH_DMA, LAUNCH_FILL);
}

static struct command_list fill_commands(uint32_t copy_class, uint64_t va, uint32_t value) {
    struct command_list list = {0};
    push_fill(&list, copy_class, va, value);
    return list;
}

// Runs MEM_OP_D's `operation` with MEM_OP_C's `fields`, MEM_OP_A and MEM_OP_B left 0
static void push_memory_operation(struct command_list* list, uint32_t fields, uint32_t operation) {
    list->words[list->count++] = method_header(MEM_OP_A, 4);
    list->words[list->count++] = 0;
    list->words[list->count++] = 0;
    list->words[list->count++] = fields;
    list->words[list->count++] = operation;
}

// Holds the methods after it until the word at `va` equals `value`
static void push_acquire(struct command_list* list, uint64_t va, uint32_t value) {
    list->words[list->count++] = method_header(SEMAPHORE_A, 4);
    list->words[list->count++] = (uint32_t)(va >> 32);
    list->words[list->count++] = (uint32_t)va;
    list->words[list->count++] = value;
    list->words[list->count++] = SEMAPHORE_ACQUIRE;
}

// Has the copy engine copy `bytes` from `from` to `to`
static struct command_list copy_commands(uint32_t copy_class, uint64_t from, uint64_t to, uint32_t bytes) {
    struct command_list list = {0};
    push_method(&list, SET_OBJECT, copy_class);
    push_address(&list, OFFSET_IN_UPPER, from);
    push_address(&list, OFFSET_OUT_UPPER, to);

    push_method(&list, LINE_LENGTH_IN, bytes);
    push_method(&list, LAUNCH_DMA, LAUNCH_COPY);

    return list;
}

// Has the copy engine copy the page at physical address 0 of video memory to `to` and to `other`. When the two hold
// different words, a copy that ran changes one of them, whatever that page holds
static struct command_list physical_copy_commands(uint32_t copy_class, uint64_t to, uint64_t other) {
    struct command_list list = {0};
    push_method(&list, SET_OBJECT, copy_class);
    push_method(&list, SET_SRC_PHYS_MODE, PHYS_MODE_VIDEO_MEMORY);
    push_address(&list, OFFSET_IN_UPPER, 0);
    push_method(&list, LINE_LENGTH_IN, (uint32_t)PAGE_BYTES);

    push_address(&list, OFFSET_OUT_UPPER, to);
    push_method(&list, LAUNCH_DMA, LAUNCH_FROM_PHYSICAL);

    push_address(&list, OFFSET_OUT_UPPER, other);
    push_method(&list, LAUNCH_DMA, LAUNCH_FROM_PHYSICAL);

    return list;
}

// Opens the session's next queue with its timeline and its page of command words. Returns false, with neither left
// open, when a call failed
static bool open_queue(struct copy_session* s) {
    struct gpu_timeline_args timeline = {0};
    if (gpu_call(s->fd, GPU_TIMELINE_CREATE, &timeline) != 0) {
        return false;
    }

    struct gpu_queue_args queue = {0};
    queue.context = s->context;
    queue.kind = GPU_QUEUE_COPY;
    if (gpu_call(s->fd, GPU_QUEUE_CREATE, &queue) != 0) {
        close(timeline.timeline);
        return false;
    }

    struct copy_queue* q = &s->queues[s->queue_count];
    q->index = queue.queue;
    q->timeline = timeline.timeline;
    q->words_va = COMMANDS_VA + s->queue_count * PAGE_BYTES;
    q->words = s->command_words + s->queue_count * PAGE_WORDS;
    s->queue_count++;

    return true;
}

// Creates the session's context, buffers and queues. Returns false when a step failed, leaving what it made for
// close_copy_session()
static bool open_copy_session(struct copy_session* s) {
    s->context = create_context(s->fd);
    create_buffer(s->fd, BUFFER_BYTES, GPU_PLACEMENT_SYSTEM, GPU_BUFFER_MAPPABLE, &s->commands);
    create_buffer(s->fd, BUFFER_BYTES, GPU_PLACEMENT_VIDEO, GPU_BUFFER_MAPPABLE, &s->video);
    create_buffer(s->fd, BUFFER_BYTES, GPU_PLACEMENT_SYSTEM, GPU_BUFFER_MAPPABLE, &s->system);

    s->command_words = map_for_cpu(s->commands);
    s->video_words = map_for_cpu(s->video);
    s->system_words = map_for_cpu(s->system);
    bool maps_commands = map(s->fd, s->context, s->commands, COMMANDS_VA, GPU_MAP_READ_ONLY) == 0;
    bool maps_video = map(s->fd, s->context, s->video, VIDEO_VA, 0) == 0;
    bool maps_system = map(s->fd, s->context, s->system, SYSTEM_VA, 0) == 0;

    bool has_cpu_views = s->command_words && s->video_words && s->system_words;
    if (!has_cpu_views || !maps_commands || !maps_video || !maps_system) {
        return false;
    }

    for (int i = 0; i < QUEUE_COUNT; i++) {
        if (!open_queue(s)) {
            return false;
        }
    }

    return true;
}

static void close_copy_session(struct copy_session* s) {
    for (uint32_t i = 0; i < s->queue_count; i++) {
        close(s->queues[i].timeline);
    }

    if (s->context >= 0) {
        close(s->context);
    }

    release(s->commands, s->command_words);
    release(s->video, s->video_words);
    release(s->system, s->system_words);
}

// Submits `list` on `q` to signal its next point, held until `wait` is reached when one is given. Returns 0 or the
// call's errno
static int submit(
    struct copy_session* s,
    struct copy_queue* q,
    const struct command_list* list,
    const struct gpu_point* wait
) {
    for (uint32_t i = 0; i < list->count; i++) {
        q->words[i] = list->words[i];
    }

    struct gpu_nvidia_range range = {0};
    range.va = q->words_va;
    range.size = list->count * sizeof(uint32_t);
    struct gpu_point signal = {0};
    signal.timeline = q->timeline;
    signal.value = q->point + 1;

    struct gpu_submit_args args = {0};
    args.context = s->context;
    args.queue = q->index;
    args.commands = (uint64_t)(uintptr_t)&range;
    args.commands_size = sizeof(range);

    args.waits = (uint64_t)(uintptr_t)wait;
    args.wait_count = wait ? 1 : 0;
    args.signals = (uint64_t)(uintptr_t)&signal;
    args.signal_count = 1;

    int error = gpu_call(s->fd, GPU_SUBMIT, &args);
    if (error == 0) {
        q->point++;
    }

    return error;
}

// Waits for the last submission on `q` to end. Returns 0, or the wait's errno, ETIMEDOUT once `timeout_ns` passed
static int wait_for(struct copy_session* s, const struct copy_queue* q, uint64_t timeout_ns) {
    struct gpu_point point = {0};
    point.timeline = q->timeline;
    point.value = q->point;

    struct gpu_wait_args args = {0};
    args.points = (uint64_t)(uintptr_t)&point;
    args.deadline_ns = monotonic_ns() + timeout_ns;
    args.count = 1;

    return gpu_call(s->fd, GPU_TIMELINE_WAIT, &args);
}

static int run(struct copy_session* s, struct copy_queue* q, const struct command_list* list) {
    int error = submit(s, q, list, NULL);
    return error != 0 ? error : wait_for(s, q, WAIT_NS);
}

// Whether `q`'s timeline reached its last point with GPU_TIMELINE_FAILED, false when the query fails
static bool ended_failed(struct copy_session* s, const struct copy_queue* q) {
    struct gpu_timeline_args query = {0};
    query.timeline = q->timeline;
    if (gpu_call(s->fd, GPU_TIMELINE_QUERY, &query) != 0) {
        return false;
    }

    return query.value >= q->point && (query.flags & GPU_TIMELINE_FAILED) != 0;
}

// Returns the context's GPU_LOST_* reason, 0 while it is live, or UINT32_MAX when the query fails
static uint32_t lost_reason(struct copy_session* s) {
    struct gpu_context_args query = {0};
    query.context = s->context;
    return gpu_call(s->fd, GPU_CONTEXT_QUERY, &query) == 0 ? query.lost : UINT32_MAX;
}

// Whether `q`'s last submission, when `submitted`, stalls past WAIT_NS and then ends failed once its deadline loses the
// context as GPU_LOST_HUNG, printing `what` with the context's GPU_LOST_* reason and the time waited
static bool stalls_until_lost(struct copy_session* s, const struct copy_queue* q, bool submitted, const char* what) {
    uint64_t start_ns = monotonic_ns();
    bool stalled = submitted && wait_for(s, q, WAIT_NS) == ETIMEDOUT;
    bool ended = stalled && wait_for(s, q, LOST_WAIT_NS) == 0;
    uint64_t elapsed_ms = (monotonic_ns() - start_ns) / NS_PER_MS;

    uint32_t lost = lost_reason(s);
    printf("gputest: %s's context reads lost %u after %llu ms\n", what, lost, (unsigned long long)elapsed_ms);

    return ended && ended_failed(s, q) && lost == GPU_LOST_HUNG;
}

static void test_fills(struct copy_session* s) {
    struct copy_queue* q = &s->queues[0];
    struct command_list video_fill = fill_commands(s->copy_class, VIDEO_VA, FILL_WORD);
    bool fills_video = run(s, q, &video_fill) == 0 && reads_as(s->video_words, FILL_WORD);
    check("a copy queue fills video memory", fills_video);

    struct command_list system_fill = fill_commands(s->copy_class, SYSTEM_VA, ~FILL_WORD);
    bool fills_system = run(s, q, &system_fill) == 0 && reads_as(s->system_words, ~FILL_WORD);
    check("a copy queue fills system memory", fills_system);
}

// The CPU reads the fill through the process's own mapping of the shared memory, so the GPU reached its pages
static void test_shared_memory_fill(struct copy_session* s) {
    int shared = memfd_create("gputest", 0);
    bool sized = shared >= 0 && ftruncate(shared, BUFFER_BYTES) == 0;
    volatile uint32_t* words = sized ? map_for_cpu(shared) : NULL;

    struct gpu_buffer_args args = {0};
    args.source = shared;
    bool imported = words && gpu_call(s->fd, GPU_BUFFER_IMPORT, &args) == 0;
    bool mapped = imported && map(s->fd, s->context, args.buffer, SHARED_VA, 0) == 0;

    struct command_list shared_fill = fill_commands(s->copy_class, SHARED_VA, FILL_WORD);
    bool filled = mapped && run(s, &s->queues[0], &shared_fill) == 0 && reads_as(words, FILL_WORD);
    check("a copy queue fills imported shared memory, and the CPU reads the fill", filled);

    if (mapped) {
        unmap(s->fd, s->context, SHARED_VA);
    }

    release(imported ? args.buffer : -1, words);
    if (shared >= 0) {
        close(shared);
    }
}

static void test_copies(struct copy_session* s) {
    struct copy_queue* q = &s->queues[0];
    struct command_list to_system = copy_commands(s->copy_class, VIDEO_VA, SYSTEM_VA, BUFFER_BYTES);
    bool copies_out = run(s, q, &to_system) == 0 && reads_as(s->system_words, FILL_WORD);
    check("a copy queue copies video memory to system memory", copies_out);

    write_indices(s->system_words);
    struct command_list to_video = copy_commands(s->copy_class, SYSTEM_VA, VIDEO_VA, BUFFER_BYTES);
    bool copies_in = run(s, q, &to_video) == 0 && holds_indices(s->video_words);
    check("a copy queue copies what the CPU wrote in system memory to video memory, word for word", copies_in);
}

// The copy goes in first, held for the other queue's fill of the memory it copies, so a copy run early would copy
// the indices there before
static void test_held_copy(struct copy_session* s) {
    struct copy_queue* filler = &s->queues[0];
    struct copy_queue* copier = &s->queues[1];
    struct gpu_point filled = {0};
    filled.timeline = filler->timeline;
    filled.value = filler->point + 1;

    struct command_list copy = copy_commands(s->copy_class, VIDEO_VA, SYSTEM_VA, BUFFER_BYTES);
    struct command_list refill = fill_commands(s->copy_class, VIDEO_VA, ~FILL_WORD);
    bool submitted = submit(s, copier, &copy, &filled) == 0 && submit(s, filler, &refill, NULL) == 0;
    bool copied = submitted && wait_for(s, copier, WAIT_NS) == 0 && reads_as(s->system_words, ~FILL_WORD);
    check("a copy held for another queue's fill runs after it and copies what the fill wrote", copied);
}

// Each copy is submitted only once the CPU saw the one before it end, so a completion the driver loses shows as a
// wait past its deadline instead of hiding behind a later release
static void test_waited_copies(struct copy_session* s) {
    struct copy_queue* q = &s->queues[0];
    struct command_list copy = copy_commands(s->copy_class, SYSTEM_VA, VIDEO_VA, sizeof(uint32_t));
    uint64_t start_ns = monotonic_ns();
    uint32_t copied = 0;
    int error = 0;

    for (uint32_t value = 1; value <= WAITED_COPIES; value++) {
        s->system_words[0] = value;
        error = run(s, q, &copy);
        if (error != 0 || s->video_words[0] != value) {
            break;
        }

        copied++;
    }

    uint64_t elapsed_ms = (monotonic_ns() - start_ns) / NS_PER_MS;
    bool copied_all = copied == WAITED_COPIES;
    printf("gputest: %u waited copies took %llu ms\n", copied, (unsigned long long)elapsed_ms);
    if (!copied_all) {
        printf("gputest: waited copy %u: %s, read back 0x%x\n", copied + 1, strerror(error), s->video_words[0]);
    }

    check("a million copies on one queue, each waited for with a 100 ms deadline, end and read back", copied_all);
}

// The host methods a user queue may use, NVK's invalidate of the GPU's cache of system memory among them
static void test_host_methods(struct copy_session* s) {
    struct copy_queue* q = &s->queues[0];
    struct command_list list = {0};
    push_method(&list, SET_REFERENCE, 1);
    push_memory_operation(&list, 0, MEM_OP_L2_SYSMEM_INVALIDATE);
    push_memory_operation(&list, 0, MEM_OP_MEMBAR);
    push_fill(&list, s->copy_class, SYSTEM_VA, FILL_WORD);

    bool ran = run(s, q, &list) == 0 && reads_as(s->system_words, FILL_WORD);
    check("SET_REFERENCE, L2_SYSMEM_INVALIDATE and MEMBAR run on a user queue ahead of its fill", ran);
}

static void test_acquire(struct copy_session* s) {
    struct copy_queue* q = &s->queues[0];
    volatile uint32_t* semaphore = s->command_words + QUEUE_COUNT * PAGE_WORDS;
    struct command_list list = {0};
    push_acquire(&list, SEMAPHORE_VA, 1);
    push_fill(&list, s->copy_class, SYSTEM_VA, ~FILL_WORD);

    bool submitted = submit(s, q, &list, NULL) == 0;
    usleep(HOLD_US);
    bool held = reads_as(s->system_words, FILL_WORD);

    // Written even when the fill ran early, so the queue is never left waiting for the checks after this one
    *semaphore = 1;
    __sync_synchronize();
    bool filled = submitted && wait_for(s, q, WAIT_NS) == 0 && reads_as(s->system_words, ~FILL_WORD);
    check("a semaphore acquire holds a user queue's fill until the CPU writes the word it waits for", held && filled);
}

// A user queue's TLB invalidate of every page directory lets the rest of its submission run, but the queue runs no
// later submission, so the next one's deadline loses the context, and the GPU runs on for other contexts
static void test_tlb_invalidate(int fd, struct copy_session* live) {
    struct gpu_memory before = read_memory(fd);
    struct copy_session invalidating = {0};
    invalidating.fd = fd;
    invalidating.copy_class = live->copy_class;

    struct copy_queue* q = &invalidating.queues[0];
    struct command_list list = {0};
    push_memory_operation(&list, TLB_INVALIDATE_ALL_PDBS, MEM_OP_TLB_INVALIDATE);
    push_fill(&list, invalidating.copy_class, VIDEO_VA, FILL_WORD);

    bool opened = open_copy_session(&invalidating);
    bool ran = opened && run(&invalidating, q, &list) == 0 && reads_as(invalidating.video_words, FILL_WORD);
    check(
        "a user queue's submission runs on past an MMU_TLB_INVALIDATE to the fill after it, its context live",
        ran && lost_reason(&invalidating) == 0
    );

    struct command_list system_fill = fill_commands(invalidating.copy_class, SYSTEM_VA, FILL_WORD);
    bool submitted = ran && submit(&invalidating, q, &system_fill, NULL) == 0;
    bool hung = stalls_until_lost(&invalidating, q, submitted, "the TLB invalidate");
    check(
        "a user queue's next submission after an MMU_TLB_INVALIDATE writes nothing, and its deadline loses the context",
        hung && reads_as(invalidating.system_words, 0)
    );

    struct command_list live_fill = fill_commands(live->copy_class, VIDEO_VA, FILL_WORD);
    bool filled = run(live, &live->queues[0], &live_fill) == 0 && reads_as(live->video_words, FILL_WORD);
    check(
        "another context's queue still fills memory once the TLB invalidate's context is lost",
        filled && hung && lost_reason(live) == 0
    );

    close_copy_session(&invalidating);
    check("closing the TLB invalidate's context gives its memory back", memory_returns_to(fd, before));
}

// A user queue's channel is denied physical addresses, so the copy stalls its queue with nothing written until the
// queue's deadline loses its context, and the GPU runs on for other contexts
static void test_physical_copy(int fd, struct copy_session* live) {
    struct gpu_memory before = read_memory(fd);
    struct copy_session denied = {0};
    denied.fd = fd;
    denied.copy_class = live->copy_class;

    bool opened = open_copy_session(&denied);
    if (opened) {
        fill(denied.video_words, FILL_WORD);
    }

    struct copy_queue* q = &denied.queues[0];
    struct command_list copy = physical_copy_commands(denied.copy_class, VIDEO_VA, SYSTEM_VA);

    bool submitted = opened && submit(&denied, q, &copy, NULL) == 0;
    bool hung = stalls_until_lost(&denied, q, submitted, "the physical copy");
    bool wrote_nothing = opened && reads_as(denied.video_words, FILL_WORD) && reads_as(denied.system_words, 0);
    check(
        "a physical-mode copy from a user queue writes nothing, and its queue's deadline loses the context",
        hung && wrote_nothing
    );

    struct command_list refill = fill_commands(live->copy_class, SYSTEM_VA, FILL_WORD);
    bool filled = run(live, &live->queues[0], &refill) == 0 && reads_as(live->system_words, FILL_WORD);
    check(
        "another context's queue still fills memory once that context is lost",
        filled && hung && lost_reason(live) == 0
    );

    close_copy_session(&denied);
    check("closing the lost context gives its memory back", memory_returns_to(fd, before));
}

static void test_session_end(struct copy_session* s, struct gpu_memory before) {
    check("the context stays live through every check on its queues", lost_reason(s) == 0);

    struct gpu_queue_args second = {0};
    second.context = s->context;
    second.queue = s->queues[1].index;
    bool destroyed = gpu_call(s->fd, GPU_QUEUE_DESTROY, &second) == 0;

    close_copy_session(s);
    bool returned = memory_returns_to(s->fd, before);
    check("destroying one queue and closing the context with the other gives their memory back", destroyed && returned);
}

static void test_copy_queues(int fd, const struct gpu_info* info) {
    struct gpu_memory before = read_memory(fd);
    struct copy_session s = {0};
    s.fd = fd;
    s.copy_class = info->nvidia.class_copy;

    bool opened = open_copy_session(&s);
    check("a context with its buffers mapped opens two copy queues", opened);
    if (!opened) {
        close_copy_session(&s);
        return;
    }

    test_fills(&s);
    test_shared_memory_fill(&s);
    test_copies(&s);
    test_held_copy(&s);
    test_waited_copies(&s);

    test_host_methods(&s);
    test_acquire(&s);
    test_tlb_invalidate(fd, &s);
    test_physical_copy(fd, &s);

    test_session_end(&s, before);
}

static int report(void) {
    printf("gputest: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    bool proto_only = argc == 2 && strcmp(argv[1], "proto") == 0;
    if (argc > 1 && !proto_only) {
        printf("usage: gputest [proto]\n");
        return 1;
    }

    if (proto_only) {
        run_proto_checks();
        return report();
    }

    int fd = open("/dev/gpu0", O_RDWR);
    if (fd < 0) {
        printf("gputest: /dev/gpu0 cannot be opened: %s\n", strerror(errno));
        return 1;
    }

    struct gpu_info info = {0};
    int error = gpu_call(fd, GPU_GET_INFO, &info);
    if (error != 0) {
        printf("gputest: GPU_GET_INFO failed: %s\n", strerror(error));
        close(fd);
        return 1;
    }

    print_facts(&info);
    test_facts(&info);
    test_free_memory(&info, read_memory(fd));
    test_clock(fd);

    test_video_buffer(fd);
    test_system_buffer(fd);
    test_scanout_buffer(fd);
    test_window_refusal(fd);

    test_context_mappings(fd);
    test_shared_memory_import(fd);
    test_many_contexts(fd);
    test_copy_queues(fd, &info);

    run_proto_checks();
    close(fd);

    return report();
}
