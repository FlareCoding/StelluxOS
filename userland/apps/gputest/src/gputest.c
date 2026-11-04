// Checks /dev/gpu0 on an NVIDIA card: its facts, its clock, its buffers, shared memory it imports, and contexts with
// their mappings. Prints a line per check and exits nonzero when any failed

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

#define NS_PER_SEC      1000000000ULL
#define MIB             (1024ULL * 1024ULL)
#define PAGE_BYTES      4096ULL
#define USER_VA_START   PAGE_BYTES
#define USER_VA_END     (1ULL << 39)
#define BUFFER_BYTES    MIB
#define BUFFER_WORDS    (BUFFER_BYTES / sizeof(uint32_t))
#define CLOCK_SLEEP_US  (100 * 1000)
#define RELEASE_WAIT_NS (2 * NS_PER_SEC)
#define POLL_US         1000
#define CONTEXT_COUNT   16
#define FILL_WORD       0xA5C3E10Fu

static int passed = 0;
static int failed = 0;

static void check(const char* name, bool cond) {
    if (cond) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

static uint64_t monotonic_ns(void) {
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
    check("the GPU is NVIDIA's", is_nvidia);

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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

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

    close(fd);
    printf("gputest: %d passed, %d failed\n", passed, failed);

    return failed > 0 ? 1 : 0;
}
