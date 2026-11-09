// Holds definitions re-declared from NVIDIA's open GPU kernel modules and open GPU documentation, under
// kernel/drivers/graphics/nvidia/LICENSE-NVIDIA

// Checks /dev/gpu0 on an NVIDIA card or on virtio-gpu with venus, then the window protocol and its GPU buffers. Exits 1
// on failure. `proto`, `resize` or `gpu` runs only its own checks, `window` the protocol and window checks without the
// GPU checks, and `display` runs the GPU checks while presenting

// Started as /bin/stlxdm with no argument, it runs only the remap check: two contexts remap buffers while gfxfb's are
// created and destroyed. A round is one remap, fill and copy on one context

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
#include "virtio_gpu.h"

#define NS_PER_MS       1000000ULL
#define NS_PER_US       1000ULL
#define PAGE_WORDS      (PAGE_BYTES / sizeof(uint32_t))
#define WORD_COUNT(a)   ((uint32_t)(sizeof(a) / sizeof(uint32_t)))
#define USER_VA_START   PAGE_BYTES
#define USER_VA_END     (1ULL << 39)
#define BUFFER_WORDS    (BUFFER_BYTES / sizeof(uint32_t))
#define CLOCK_SLEEP_US  (100 * 1000)
#define RELEASE_WAIT_NS (2 * NS_PER_SEC)
#define CONTEXT_COUNT   16
#define QUEUE_COUNT     2
#define LIST_WORDS_MAX  256
#define WAIT_NS         (100 * NS_PER_MS)
#define WAITED_COPIES   1000000u
#define HOLD_US         (20 * 1000)
#define LOST_WAIT_NS    (GPU_SUBMIT_TIMEOUT_NS + 2 * NS_PER_SEC)
#define STORM_METHODS   250000u
#define REMAP_CONTEXTS  2
#define REMAP_NS        (10 * NS_PER_SEC)
#define DEVICE_WAIT_NS  (30 * NS_PER_SEC)
#define STLXDM_PATH     "/bin/stlxdm" // where init starts the display manager

// The copy part's buffers in its context: the first holds a page of command words per queue, the word a semaphore
// acquire reads and an interrupt storm's words, and the next three the memory they fill and copy
#define COMMANDS_VA  USER_VA_START
#define SEMAPHORE_VA (COMMANDS_VA + QUEUE_COUNT * PAGE_BYTES)
#define STORM_VA     (SEMAPHORE_VA + PAGE_BYTES)
#define VIDEO_VA     (COMMANDS_VA + BUFFER_BYTES)
#define SYSTEM_VA    (VIDEO_VA + BUFFER_BYTES)
#define SHARED_VA    (SYSTEM_VA + BUFFER_BYTES)

// The copy part maps no buffer here, so a fill of it faults its channel
#define UNMAPPED_VA (SHARED_VA + BUFFER_BYTES)

// The remap check's second video buffer in each context, and where each round maps one of the context's two in turn
#define OTHER_VIDEO_VA (UNMAPPED_VA + BUFFER_BYTES)
#define REMAP_VA       (OTHER_VIDEO_VA + BUFFER_BYTES)

// NVIDIA's NVC56F_DMA_SEC_OP_INC_METHOD and NON_INC_METHOD push buffer headers, whose values go to consecutive methods
// or all to one, with the value count from bit 16 and the subchannel from bit 13
#define PUSH_HEADER_INCREASING       (1u << 29)
#define PUSH_HEADER_SAME_METHOD      (3u << 29)
#define PUSH_HEADER_COUNT_SHIFT      16
#define PUSH_HEADER_SUBCHANNEL_SHIFT 13

// NVIDIA's NVC56F_DMA_SEC_OP_IMMD_DATA_METHOD, a push buffer word holding one method and its value from bit 16
#define PUSH_IMMEDIATE (4u << 29)

// NVIDIA's NVC56F host methods: SET_OBJECT (which binds a class to the subchannel), SEMAPHOREA to SEMAPHORED,
// NON_STALL_INTERRUPT, MEM_OP_A to MEM_OP_D (which run MEM_OP_D's operation) and SET_REFERENCE
#define SET_OBJECT          0x0000u
#define SEMAPHORE_A         0x0010u
#define NON_STALL_INTERRUPT 0x0020u
#define MEM_OP_A            0x0028u
#define SET_REFERENCE       0x0050u

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

// NVIDIA's NVA06F_SUBCHANNEL_3D, _COMPUTE and _COPY_ENGINE, the subchannels of a graphics channel's classes
#define SUBCHANNEL_3D      0u
#define SUBCHANNEL_COMPUTE 1u
#define SUBCHANNEL_COPY    4u

// NVIDIA's NVC797 3D methods: color target 0's nine from SET_COLOR_TARGET_A, the clear rectangle's, surface clip's and
// window offset's two, the clear color's four, and CLEAR_SURFACE, which clears the rectangle of the selected targets
#define SET_COLOR_TARGET_A          0x0800u
#define SET_CLEAR_RECT_HORIZONTAL   0x0D6Cu
#define SET_COLOR_CLEAR_VALUE       0x0D80u
#define SET_WINDOW_OFFSET_X         0x0DF8u
#define SET_SURFACE_CLIP_HORIZONTAL 0x0FF4u
#define SET_CLEAR_SURFACE_CONTROL   0x10F8u
#define SET_CT_SELECT               0x121Cu
#define SET_ZT_SELECT               0x1538u
#define SET_RENDER_ENABLE_C         0x1558u
#define CLEAR_SURFACE               0x19D0u
#define SET_COLOR_COMPRESSION       0x19E0u
#define SET_CT_WRITE                0x1A00u

// NVIDIA's NVC797 fields: SET_COLOR_TARGET_FORMAT A8B8G8R8, SET_COLOR_TARGET_MEMORY LAYOUT_PITCH, SET_CT_WRITE and
// CLEAR_SURFACE with R, G, B and A enabled, SET_CLEAR_SURFACE_CONTROL USE_CLEAR_RECT and SET_RENDER_ENABLE_C MODE_TRUE
#define COLOR_FORMAT_A8B8G8R8 0xD5u
#define COLOR_LAYOUT_PITCH    (1u << 12)
#define CT_WRITE_RGBA         0x1111u
#define CLEAR_RGBA            (0xFu << 2)
#define CLEAR_USE_RECT        (1u << 4)
#define RENDER_ENABLE_TRUE    1u

// NVIDIA's NVC7C0 compute methods: the shared and local memory windows, upper word first, SEND_PCAS_A, a QMD's address
// shifted right by 8, and SEND_SIGNALING_PCAS2_B, whose PCAS_ACTION INVALIDATE_COPY_SCHEDULE launches the QMD's grid
#define SET_SHADER_SHARED_MEMORY_WINDOW_A 0x02A0u
#define SEND_PCAS_A                       0x02B4u
#define SEND_SIGNALING_PCAS2_B            0x02C0u
#define SET_SHADER_LOCAL_MEMORY_WINDOW_A  0x07B0u
#define PCAS_INVALIDATE_COPY_SCHEDULE     0x3u

// Where a compute grid's threads see shared and local memory, above every buffer the checks map
#define SHARED_WINDOW_VA 0xFE000000ULL
#define LOCAL_WINDOW_VA  0xFF000000ULL

// Fields of NVIDIA's QMD version 3.0 (NVC6C0_QMDV03_00), the descriptor of a grid a compute class launches, each by the
// bit it starts at, and every field the checks set lying within one 32-bit word
#define QMD_WORDS                            64
#define QMD_BYTES                            (QMD_WORDS * sizeof(uint32_t))
#define QMD_SM_GLOBAL_CACHING_ENABLE         134
#define QMD_API_VISIBLE_CALL_LIMIT           378
#define QMD_CTA_RASTER_WIDTH                 384
#define QMD_CTA_RASTER_HEIGHT                416
#define QMD_CTA_RASTER_DEPTH                 448
#define QMD_MIN_SM_CONFIG_SHARED_MEM_SIZE    562
#define QMD_MAX_SM_CONFIG_SHARED_MEM_SIZE    569
#define QMD_MAJOR_VERSION                    580
#define QMD_CTA_THREAD_DIMENSION0            592
#define QMD_CTA_THREAD_DIMENSION1            608
#define QMD_CTA_THREAD_DIMENSION2            624
#define QMD_REGISTER_COUNT_V                 648
#define QMD_TARGET_SM_CONFIG_SHARED_MEM_SIZE 657
#define QMD_RELEASE0_ADDRESS_LOWER           768
#define QMD_RELEASE0_ADDRESS_UPPER           800
#define QMD_RELEASE0_MEMBAR_TYPE             819
#define QMD_RELEASE0_ENABLE                  823
#define QMD_RELEASE0_STRUCTURE_SIZE          830
#define QMD_RELEASE0_PAYLOAD_LOWER           832
#define QMD_PROGRAM_ADDRESS_LOWER            1536
#define QMD_PROGRAM_ADDRESS_UPPER            1568

// NVIDIA's QMD values: major version 3, NO_CHECK, FE_SYSMEMBAR and SEMAPHORE_ONE_WORD. A shared memory configuration
// of n KiB is n / 4 + 1, from none to the 100 KiB an Ampere GA10x SM offers at most
#define QMD_VERSION_3        3u
#define CALL_LIMIT_NO_CHECK  1u
#define RELEASE_FE_SYSMEMBAR 1u
#define RELEASE_ONE_WORD     1u
#define SHARED_MEMORY_NONE   1u
#define SHARED_MEMORY_MOST   26u

// The registers a program's threads take: R0 to R3 and the two the hardware keeps for the program counter
#define PROGRAM_REGISTERS 6u

// The last word of an Ampere instruction: the cycles the warp stalls after it, yield, and no barrier set or waited for
#define SCHEDULE(stall_cycles) (((stall_cycles) << 9) | (1u << 13) | (7u << 14) | (7u << 17))

// The graphics part's video buffer holds its grids' QMDs, each program on a page of its own with zeros after it for
// the SMs' instruction prefetch, and the words the grids write
#define STORE_QMD_VA     VIDEO_VA
#define EXIT_QMD_VA      (VIDEO_VA + QMD_BYTES)
#define STORE_PROGRAM_VA (VIDEO_VA + PAGE_BYTES)
#define EXIT_PROGRAM_VA  (VIDEO_VA + 2 * PAGE_BYTES)
#define STORED_VA        (VIDEO_VA + 3 * PAGE_BYTES)
#define GRID_RELEASE_VA  (STORED_VA + sizeof(uint32_t))

// The clears' target, the system buffer as rows of 512 A8B8G8R8 pixels
#define TARGET_WIDTH  512u
#define TARGET_PITCH  (TARGET_WIDTH * (uint32_t)sizeof(uint32_t))
#define TARGET_HEIGHT ((uint32_t)(BUFFER_BYTES / TARGET_PITCH))

// The words the graphics checks wait for. The target is cleared to magenta many times and then green, keeping the
// 3D pipe busy for milliseconds, and the exit grid's 2^26 CTAs (thread blocks) keep the compute pipe busy longer
#define STORED_WORD       0x5CA1AB1Eu
#define GRID_RELEASE_WORD 0x0DDBA11u
#define LAST_CLEAR_WORD   0xFF00FF00u
#define FLOAT_ONE         0x3F800000u
#define CLEAR_REPEATS     64u
#define GRID_CTAS         (1u << 26)
#define GRID_THREADS      32u
#define GRAPHICS_WAIT_NS  (2 * NS_PER_SEC)

// Method words, each sent to the class bound to `subchannel`, which a submission copies to its queue's command words
struct command_list {
    uint32_t count;
    uint32_t subchannel;
    uint32_t words[LIST_WORDS_MAX];
};

// A queue with its timeline and its command words, a page of the session's command buffer
struct queue {
    uint32_t           index;
    int32_t            timeline;
    uint64_t           point; // the value its last submission signals
    uint64_t           words_va;
    volatile uint32_t* words;
};

// A context with queues of one kind and three buffers mapped for the CPU and the GPU: the queues' command words,
// read-only to the GPU, then video and system memory for their work, each check starting from what the last left
struct session {
    int                fd;
    uint32_t           copy_class;
    int32_t            context;
    struct queue       queues[QUEUE_COUNT];
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

// The clear colors' red, green, blue and alpha as floats
static const uint32_t FIRST_CLEAR_COLOR[] = {FLOAT_ONE, 0, FLOAT_ONE, FLOAT_ONE};
static const uint32_t LAST_CLEAR_COLOR[] = {0, FLOAT_ONE, 0, FLOAT_ONE};

// The store grid's program, Ampere instructions of four words each, run on predicate PT. A MOV stalls long enough for
// the store to read the register it sets
static const uint32_t STORE_PROGRAM[] = {
    0x00007802u, STORED_WORD,                 0x00000F00u, SCHEDULE(15), // MOV R0, STORED_WORD
    0x00027802u, (uint32_t)STORED_VA,         0x00000F00u, SCHEDULE(15), // MOV R2, STORED_VA's lower word
    0x00037802u, (uint32_t)(STORED_VA >> 32), 0x00000F00u, SCHEDULE(15), // MOV R3, its upper word
    0x02007986u, 0x00000000u,                 0x0C10093Fu, SCHEDULE(1),  // STG.E [R2.64], R0
    0x0000794Du, 0x00000000u,                 0x03800000u, SCHEDULE(5),  // EXIT
};

// The exit grid's program, EXIT alone
static const uint32_t EXIT_PROGRAM[] = {0x0000794Du, 0x00000000u, 0x03800000u, SCHEDULE(5)};

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

int gpu_call(int fd, uint32_t cmd, void* arg) {
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

volatile uint32_t* map_for_cpu(int32_t buffer) {
    void* words = mmap(NULL, BUFFER_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, buffer, 0);
    return words == MAP_FAILED ? NULL : words;
}

bool reads_as(const volatile uint32_t* words, uint32_t value) {
    for (size_t i = 0; i < BUFFER_WORDS; i++) {
        if (words[i] != value) {
            return false;
        }
    }

    return true;
}

// On x86_64 the fence drains the CPU's write-combining buffers, so a later read sees memory
void fill(volatile uint32_t* words, uint32_t value) {
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

void release(int32_t buffer, volatile uint32_t* words) {
    if (words) {
        munmap((void*)words, BUFFER_BYTES);
    }

    if (buffer >= 0) {
        close(buffer);
    }
}

int32_t create_context(int fd) {
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
    bool graphics_and_copy = info->queue_kinds == ((1u << GPU_QUEUE_GRAPHICS) | (1u << GPU_QUEUE_COPY));
    check("the GPU is NVIDIA's and offers graphics and copy queues", is_nvidia && graphics_and_copy);

    uint32_t video_and_system = (1u << GPU_PLACEMENT_VIDEO) | (1u << GPU_PLACEMENT_SYSTEM);
    bool user_range = info->va_start == USER_VA_START && info->va_end == USER_VA_END;
    bool small_pages = info->page_size == PAGE_BYTES;
    check(
        "it offers video and system memory and maps user addresses from 4 KiB to 2^39 in 4 KiB pages",
        info->placements == video_and_system && user_range && small_pages
    );

    bool named = info->name[0] != '\0';
    bool has_classes = info->nvidia.class_gpfifo != 0 && info->nvidia.class_copy != 0;
    bool has_2d_and_3d = info->nvidia.class_2d != 0 && info->nvidia.class_3d != 0;
    bool has_m2mf_and_compute = info->nvidia.class_m2mf != 0 && info->nvidia.class_compute != 0;
    bool counts_shaders = info->nvidia.gpc_count != 0 && info->nvidia.tpc_count >= info->nvidia.gpc_count;
    check(
        "it names its product, its channel, copy and graphics engine classes, and counts its shader units",
        named && has_classes && has_2d_and_3d && has_m2mf_and_compute && counts_shaders
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

// A push buffer header sending `count` values to consecutive methods from `method`, on the list's subchannel
static uint32_t method_header(const struct command_list* list, uint32_t method, uint32_t count) {
    uint32_t subchannel = list->subchannel << PUSH_HEADER_SUBCHANNEL_SHIFT;
    return PUSH_HEADER_INCREASING | (count << PUSH_HEADER_COUNT_SHIFT) | subchannel | (method >> 2);
}

static void push_method(struct command_list* list, uint32_t method, uint32_t value) {
    list->words[list->count++] = method_header(list, method, 1);
    list->words[list->count++] = value;
}

static void push_values(struct command_list* list, uint32_t method, const uint32_t* values, uint32_t count) {
    list->words[list->count++] = method_header(list, method, count);
    for (uint32_t i = 0; i < count; i++) {
        list->words[list->count++] = values[i];
    }
}

// Sends `value` to `method` `count` times under one header
static void push_repeated(struct command_list* list, uint32_t method, uint32_t value, uint32_t count) {
    uint32_t subchannel = list->subchannel << PUSH_HEADER_SUBCHANNEL_SHIFT;
    uint32_t header = PUSH_HEADER_SAME_METHOD | (count << PUSH_HEADER_COUNT_SHIFT) | subchannel | (method >> 2);
    list->words[list->count++] = header;
    for (uint32_t i = 0; i < count; i++) {
        list->words[list->count++] = value;
    }
}

// Sends the upper word of `va` to `upper_method` and its lower word to the method after it
static void push_address(struct command_list* list, uint32_t upper_method, uint64_t va) {
    list->words[list->count++] = method_header(list, upper_method, 2);
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
    list->words[list->count++] = method_header(list, MEM_OP_A, 4);
    list->words[list->count++] = 0;
    list->words[list->count++] = 0;
    list->words[list->count++] = fields;
    list->words[list->count++] = operation;
}

// Holds the methods after it until the word at `va` equals `value`
static void push_acquire(struct command_list* list, uint64_t va, uint32_t value) {
    list->words[list->count++] = method_header(list, SEMAPHORE_A, 4);
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

// Has the 3D class clear the target CLEAR_REPEATS times to the first color and then once to the last, so it reads
// LAST_CLEAR_WORD only once every clear ran
static void push_clears(struct command_list* list, uint32_t class_3d) {
    // Color target 0's nine values from SET_COLOR_TARGET_A, the seventh giving it one layer
    uint32_t target[] = {
        (uint32_t)(SYSTEM_VA >> 32),
        (uint32_t)SYSTEM_VA,
        TARGET_PITCH,
        TARGET_HEIGHT,
        COLOR_FORMAT_A8B8G8R8,
        COLOR_LAYOUT_PITCH,
        1,
        0,
        0,
    };

    uint32_t whole_target[] = {TARGET_WIDTH << 16, TARGET_HEIGHT << 16}; // from 0, to the width and to the height
    uint32_t origin[] = {0, 0};

    list->subchannel = SUBCHANNEL_3D;
    push_method(list, SET_OBJECT, class_3d);

    push_values(list, SET_COLOR_TARGET_A, target, WORD_COUNT(target));
    push_method(list, SET_COLOR_COMPRESSION, 0);
    push_method(list, SET_CT_WRITE, CT_WRITE_RGBA);
    push_method(list, SET_CT_SELECT, 1); // one target, color target 0
    push_method(list, SET_ZT_SELECT, 0);

    push_values(list, SET_SURFACE_CLIP_HORIZONTAL, whole_target, WORD_COUNT(whole_target));
    push_values(list, SET_WINDOW_OFFSET_X, origin, WORD_COUNT(origin));
    push_values(list, SET_CLEAR_RECT_HORIZONTAL, whole_target, WORD_COUNT(whole_target));
    push_method(list, SET_CLEAR_SURFACE_CONTROL, CLEAR_USE_RECT);
    push_method(list, SET_RENDER_ENABLE_C, RENDER_ENABLE_TRUE);

    push_values(list, SET_COLOR_CLEAR_VALUE, FIRST_CLEAR_COLOR, WORD_COUNT(FIRST_CLEAR_COLOR));
    push_repeated(list, CLEAR_SURFACE, CLEAR_RGBA, CLEAR_REPEATS);
    push_values(list, SET_COLOR_CLEAR_VALUE, LAST_CLEAR_COLOR, WORD_COUNT(LAST_CLEAR_COLOR));
    push_method(list, CLEAR_SURFACE, CLEAR_RGBA);
}

// Has the compute class launch the QMD at `qmd_va`
static void push_grid(struct command_list* list, uint32_t class_compute, uint64_t qmd_va) {
    list->subchannel = SUBCHANNEL_COMPUTE;
    push_method(list, SET_OBJECT, class_compute);
    push_address(list, SET_SHADER_SHARED_MEMORY_WINDOW_A, SHARED_WINDOW_VA);
    push_address(list, SET_SHADER_LOCAL_MEMORY_WINDOW_A, LOCAL_WINDOW_VA);

    push_method(list, SEND_PCAS_A, (uint32_t)(qmd_va >> 8));
    push_method(list, SEND_SIGNALING_PCAS2_B, PCAS_INVALIDATE_COPY_SCHEDULE);
}

static void set_qmd_field(uint32_t* qmd, uint32_t bit, uint32_t value) {
    qmd[bit / 32] |= value << (bit % 32);
}

// Writes to `to` a QMD launching `ctas` CTAs of `threads` threads each running the program at `program_va`, whose
// release writes GRID_RELEASE_WORD to `release_va` once the grid ended, when `release_va` is not 0
static void write_qmd(
    volatile uint32_t* to,
    uint64_t program_va,
    uint32_t ctas,
    uint32_t threads,
    uint64_t release_va
) {
    uint32_t qmd[QMD_WORDS] = {0};
    set_qmd_field(qmd, QMD_SM_GLOBAL_CACHING_ENABLE, 1);
    set_qmd_field(qmd, QMD_API_VISIBLE_CALL_LIMIT, CALL_LIMIT_NO_CHECK);
    set_qmd_field(qmd, QMD_MAJOR_VERSION, QMD_VERSION_3);

    set_qmd_field(qmd, QMD_CTA_RASTER_WIDTH, ctas);
    set_qmd_field(qmd, QMD_CTA_RASTER_HEIGHT, 1);
    set_qmd_field(qmd, QMD_CTA_RASTER_DEPTH, 1);
    set_qmd_field(qmd, QMD_CTA_THREAD_DIMENSION0, threads);
    set_qmd_field(qmd, QMD_CTA_THREAD_DIMENSION1, 1);
    set_qmd_field(qmd, QMD_CTA_THREAD_DIMENSION2, 1);

    set_qmd_field(qmd, QMD_MIN_SM_CONFIG_SHARED_MEM_SIZE, SHARED_MEMORY_NONE);
    set_qmd_field(qmd, QMD_TARGET_SM_CONFIG_SHARED_MEM_SIZE, SHARED_MEMORY_NONE);
    set_qmd_field(qmd, QMD_MAX_SM_CONFIG_SHARED_MEM_SIZE, SHARED_MEMORY_MOST);
    set_qmd_field(qmd, QMD_REGISTER_COUNT_V, PROGRAM_REGISTERS);
    set_qmd_field(qmd, QMD_PROGRAM_ADDRESS_LOWER, (uint32_t)program_va);
    set_qmd_field(qmd, QMD_PROGRAM_ADDRESS_UPPER, (uint32_t)(program_va >> 32));

    if (release_va != 0) {
        set_qmd_field(qmd, QMD_RELEASE0_ADDRESS_LOWER, (uint32_t)release_va);
        set_qmd_field(qmd, QMD_RELEASE0_ADDRESS_UPPER, (uint32_t)(release_va >> 32));
        set_qmd_field(qmd, QMD_RELEASE0_MEMBAR_TYPE, RELEASE_FE_SYSMEMBAR);
        set_qmd_field(qmd, QMD_RELEASE0_ENABLE, 1);
        set_qmd_field(qmd, QMD_RELEASE0_STRUCTURE_SIZE, RELEASE_ONE_WORD);
        set_qmd_field(qmd, QMD_RELEASE0_PAYLOAD_LOWER, GRID_RELEASE_WORD);
    }

    for (uint32_t i = 0; i < QMD_WORDS; i++) {
        to[i] = qmd[i];
    }
}

// Opens the session's next queue, of `kind`, with its timeline and its page of command words. Returns false, with
// neither left open, when a call failed
static bool open_queue(struct session* s, uint32_t kind) {
    struct gpu_timeline_args timeline = {0};
    if (gpu_call(s->fd, GPU_TIMELINE_CREATE, &timeline) != 0) {
        return false;
    }

    struct gpu_queue_args queue = {0};
    queue.context = s->context;
    queue.kind = kind;
    if (gpu_call(s->fd, GPU_QUEUE_CREATE, &queue) != 0) {
        close(timeline.timeline);
        return false;
    }

    struct queue* q = &s->queues[s->queue_count];
    q->index = queue.queue;
    q->timeline = timeline.timeline;
    q->words_va = COMMANDS_VA + s->queue_count * PAGE_BYTES;
    q->words = s->command_words + s->queue_count * PAGE_WORDS;
    s->queue_count++;

    return true;
}

// Creates the session's context, buffers and `count` queues of `kind`. Returns false when a step failed, leaving what
// it made for close_session()
static bool open_session(struct session* s, uint32_t kind, uint32_t count) {
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

    for (uint32_t i = 0; i < count; i++) {
        if (!open_queue(s, kind)) {
            return false;
        }
    }

    return true;
}

static void close_session(struct session* s) {
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

// Submits the `count` command words at `va` on `q` to signal its next point, held until `wait` is reached when one is
// given. Returns 0 or the call's errno
static int submit_words(
    struct session* s,
    struct queue* q,
    uint64_t va,
    uint32_t count,
    const struct gpu_point* wait
) {
    struct gpu_nvidia_range range = {0};
    range.va = va;
    range.size = count * sizeof(uint32_t);
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

// Copies `list` into `q`'s command words and submits them, as submit_words() does
static int submit(
    struct session* s,
    struct queue* q,
    const struct command_list* list,
    const struct gpu_point* wait
) {
    for (uint32_t i = 0; i < list->count; i++) {
        q->words[i] = list->words[i];
    }

    return submit_words(s, q, q->words_va, list->count, wait);
}

// Waits for the last submission on `q` to end. Returns 0, or the wait's errno, ETIMEDOUT once `timeout_ns` passed
static int wait_for(struct session* s, const struct queue* q, uint64_t timeout_ns) {
    struct gpu_point point = {0};
    point.timeline = q->timeline;
    point.value = q->point;

    struct gpu_wait_args args = {0};
    args.points = (uint64_t)(uintptr_t)&point;
    args.deadline_ns = monotonic_ns() + timeout_ns;
    args.count = 1;

    return gpu_call(s->fd, GPU_TIMELINE_WAIT, &args);
}

static int run(struct session* s, struct queue* q, const struct command_list* list) {
    int error = submit(s, q, list, NULL);
    return error != 0 ? error : wait_for(s, q, WAIT_NS);
}

int32_t submit_fill(
    int fd,
    const struct gpu_info* info,
    int32_t buffer,
    uint32_t value,
    const struct gpu_point* wait,
    const struct gpu_point* signal
) {
    int32_t context = create_context(fd);
    if (context < 0) {
        return -1;
    }

    int32_t commands = -1;
    create_buffer(fd, BUFFER_BYTES, GPU_PLACEMENT_SYSTEM, GPU_BUFFER_MAPPABLE, &commands);

    volatile uint32_t* command_words = map_for_cpu(commands);
    bool maps_commands = map(fd, context, commands, COMMANDS_VA, GPU_MAP_READ_ONLY) == 0;
    bool maps_buffer = map(fd, context, buffer, SYSTEM_VA, 0) == 0;

    struct gpu_queue_args queue = {0};
    queue.context = context;
    queue.kind = GPU_QUEUE_COPY;
    bool ready = command_words && maps_commands && maps_buffer && gpu_call(fd, GPU_QUEUE_CREATE, &queue) == 0;

    struct command_list list = fill_commands(info->nvidia.class_copy, SYSTEM_VA, value);
    for (uint32_t i = 0; ready && i < list.count; i++) {
        command_words[i] = list.words[i];
    }

    struct gpu_nvidia_range range = {0};
    range.va = COMMANDS_VA;
    range.size = list.count * sizeof(uint32_t);

    struct gpu_submit_args args = {0};
    args.context = context;
    args.queue = queue.queue;
    args.commands = (uint64_t)(uintptr_t)&range;
    args.commands_size = sizeof(range);

    args.waits = (uint64_t)(uintptr_t)wait;
    args.wait_count = 1;
    args.signals = (uint64_t)(uintptr_t)signal;
    args.signal_count = 1;
    bool submitted = ready && gpu_call(fd, GPU_SUBMIT, &args) == 0;

    // The context's mapping keeps the command buffer until the context closes
    release(commands, command_words);
    if (!submitted) {
        close(context);
        return -1;
    }

    return context;
}

// Whether `q`'s timeline reached its last point with GPU_TIMELINE_FAILED, false when the query fails
static bool ended_failed(struct session* s, const struct queue* q) {
    struct gpu_timeline_args query = {0};
    query.timeline = q->timeline;
    if (gpu_call(s->fd, GPU_TIMELINE_QUERY, &query) != 0) {
        return false;
    }

    return query.value >= q->point && (query.flags & GPU_TIMELINE_FAILED) != 0;
}

// Returns the context's GPU_LOST_* reason, 0 while it is live, or UINT32_MAX when the query fails
static uint32_t lost_reason(struct session* s) {
    struct gpu_context_args query = {0};
    query.context = s->context;
    return gpu_call(s->fd, GPU_CONTEXT_QUERY, &query) == 0 ? query.lost : UINT32_MAX;
}

// Whether `q`'s last submission, when `submitted`, stalls past WAIT_NS and then ends failed once its deadline loses the
// context as GPU_LOST_HUNG, printing `what` with the context's GPU_LOST_* reason and the time waited
static bool stalls_until_lost(struct session* s, const struct queue* q, bool submitted, const char* what) {
    uint64_t start_ns = monotonic_ns();
    bool stalled = submitted && wait_for(s, q, WAIT_NS) == ETIMEDOUT;
    bool ended = stalled && wait_for(s, q, LOST_WAIT_NS) == 0;
    uint64_t elapsed_ms = (monotonic_ns() - start_ns) / NS_PER_MS;

    uint32_t lost = lost_reason(s);
    printf("gputest: %s's context reads lost %u after %llu ms\n", what, lost, (unsigned long long)elapsed_ms);

    return ended && ended_failed(s, q) && lost == GPU_LOST_HUNG;
}

static void test_fills(struct session* s) {
    struct queue* q = &s->queues[0];
    struct command_list video_fill = fill_commands(s->copy_class, VIDEO_VA, FILL_WORD);
    bool fills_video = run(s, q, &video_fill) == 0 && reads_as(s->video_words, FILL_WORD);
    check("a copy queue fills video memory", fills_video);

    struct command_list system_fill = fill_commands(s->copy_class, SYSTEM_VA, ~FILL_WORD);
    bool fills_system = run(s, q, &system_fill) == 0 && reads_as(s->system_words, ~FILL_WORD);
    check("a copy queue fills system memory", fills_system);
}

// The CPU reads the fill through the process's own mapping of the shared memory, so the GPU reached its pages
static void test_shared_memory_fill(struct session* s) {
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

static void test_copies(struct session* s) {
    struct queue* q = &s->queues[0];
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
static void test_held_copy(struct session* s) {
    struct queue* filler = &s->queues[0];
    struct queue* copier = &s->queues[1];
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
static void test_waited_copies(struct session* s) {
    struct queue* q = &s->queues[0];
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
static void test_host_methods(struct session* s) {
    struct queue* q = &s->queues[0];
    struct command_list list = {0};
    push_method(&list, SET_REFERENCE, 1);
    push_memory_operation(&list, 0, MEM_OP_L2_SYSMEM_INVALIDATE);
    push_memory_operation(&list, 0, MEM_OP_MEMBAR);
    push_fill(&list, s->copy_class, SYSTEM_VA, FILL_WORD);

    bool ran = run(s, q, &list) == 0 && reads_as(s->system_words, FILL_WORD);
    check("SET_REFERENCE, L2_SYSMEM_INVALIDATE and MEMBAR run on a user queue ahead of its fill", ran);
}

static void test_acquire(struct session* s) {
    struct queue* q = &s->queues[0];
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
static void test_tlb_invalidate(int fd, struct session* live) {
    struct gpu_memory before = read_memory(fd);
    struct session invalidating = {0};
    invalidating.fd = fd;
    invalidating.copy_class = live->copy_class;

    struct queue* q = &invalidating.queues[0];
    struct command_list list = {0};
    push_memory_operation(&list, TLB_INVALIDATE_ALL_PDBS, MEM_OP_TLB_INVALIDATE);
    push_fill(&list, invalidating.copy_class, VIDEO_VA, FILL_WORD);

    bool opened = open_session(&invalidating, GPU_QUEUE_COPY, QUEUE_COUNT);
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

    close_session(&invalidating);
    check("closing the TLB invalidate's context gives its memory back", memory_returns_to(fd, before));
}

// A user queue's channel is denied physical addresses, so the copy stalls its queue with nothing written until the
// queue's deadline loses its context, and the GPU runs on for other contexts
static void test_physical_copy(int fd, struct session* live) {
    struct gpu_memory before = read_memory(fd);
    struct session denied = {0};
    denied.fd = fd;
    denied.copy_class = live->copy_class;

    bool opened = open_session(&denied, GPU_QUEUE_COPY, QUEUE_COUNT);
    if (opened) {
        fill(denied.video_words, FILL_WORD);
    }

    struct queue* q = &denied.queues[0];
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

    close_session(&denied);
    check("closing the lost context gives its memory back", memory_returns_to(fd, before));
}

// A fill of unmapped memory faults its channel, and the firmware's report of the fault loses the context before the
// queue's deadline, while the GPU runs on for other contexts
static void test_faulting_fill(int fd, struct session* live) {
    struct gpu_memory before = read_memory(fd);
    struct session faulting = {0};
    faulting.fd = fd;
    faulting.copy_class = live->copy_class;

    struct queue* q = &faulting.queues[0];
    struct command_list unmapped_fill = fill_commands(faulting.copy_class, UNMAPPED_VA, FILL_WORD);

    bool opened = open_session(&faulting, GPU_QUEUE_COPY, QUEUE_COUNT);
    bool submitted = opened && submit(&faulting, q, &unmapped_fill, NULL) == 0;
    uint64_t start_ns = monotonic_ns();
    bool ended = submitted && wait_for(&faulting, q, LOST_WAIT_NS) == 0;
    uint64_t elapsed_ns = monotonic_ns() - start_ns;

    struct gpu_context_args query = {0};
    query.context = faulting.context;
    bool queried = gpu_call(fd, GPU_CONTEXT_QUERY, &query) == 0;
    printf(
        "gputest: the faulting fill's context reads lost %u at address 0x%llx after %llu ms\n",
        query.lost,
        (unsigned long long)query.fault_va,
        (unsigned long long)(elapsed_ns / NS_PER_MS)
    );

    bool ended_in_time = ended && elapsed_ns < GPU_SUBMIT_TIMEOUT_NS && ended_failed(&faulting, q);
    bool lost_to_fault = queried && query.lost == GPU_LOST_FAULT && query.fault_va == UNMAPPED_VA;
    check(
        "a fill of unmapped memory ends failed before its deadline, its context lost to a fault at the fill's address",
        ended_in_time && lost_to_fault
    );

    struct command_list system_fill = fill_commands(faulting.copy_class, SYSTEM_VA, FILL_WORD);
    bool refuses_submission = opened && submit(&faulting, &faulting.queues[1], &system_fill, NULL) == EIO;
    bool refuses_mapping = opened && map(fd, faulting.context, faulting.system, SHARED_VA, 0) == EIO;
    check("the faulted context refuses later submissions and mappings with EIO", refuses_submission && refuses_mapping);

    struct command_list live_fill = fill_commands(live->copy_class, SYSTEM_VA, ~FILL_WORD);
    bool filled = run(live, &live->queues[0], &live_fill) == 0 && reads_as(live->system_words, ~FILL_WORD);
    check(
        "another context's queue still fills memory once the faulting fill's context is lost",
        filled && lost_to_fault && lost_reason(live) == 0
    );

    close_session(&faulting);
    check("closing the faulted context gives its memory back", memory_returns_to(fd, before));
}

// A queue taking the faulting fill's channel id runs a fill, then an acquire of a word nobody writes holds it until the
// deadline loses the context and stops the queues, so writing the word later runs nothing, and other contexts run on
static void test_hung_acquire(int fd, struct session* live) {
    struct gpu_memory before = read_memory(fd);
    struct session hung = {0};
    hung.fd = fd;
    hung.copy_class = live->copy_class;

    struct queue* q = &hung.queues[0];
    struct command_list reused_fill = fill_commands(hung.copy_class, SYSTEM_VA, ~FILL_WORD);
    struct command_list list = {0};
    push_acquire(&list, SEMAPHORE_VA, 1);
    push_fill(&list, hung.copy_class, SYSTEM_VA, FILL_WORD);

    // The first queue takes the lowest free channel id, the one the faulting fill's queue gave back
    bool opened = open_session(&hung, GPU_QUEUE_COPY, QUEUE_COUNT);
    bool reused = opened && run(&hung, q, &reused_fill) == 0 && reads_as(hung.system_words, ~FILL_WORD);
    check("a queue taking the faulting fill's channel id runs a fill", reused);

    bool submitted = opened && submit(&hung, q, &list, NULL) == 0;
    bool lost = stalls_until_lost(&hung, q, submitted, "the hung acquire");
    check("a submission held by an acquire of a word nobody writes stalls until its deadline loses the context", lost);

    if (lost) {
        volatile uint32_t* semaphore = hung.command_words + QUEUE_COUNT * PAGE_WORDS;
        *semaphore = 1;
        __sync_synchronize();
        usleep(HOLD_US);
    }

    check(
        "the lost context's stopped queue runs nothing once the word its acquire waits for is written",
        lost && reads_as(hung.system_words, ~FILL_WORD)
    );

    struct command_list live_fill = fill_commands(live->copy_class, SYSTEM_VA, FILL_WORD);
    bool filled = run(live, &live->queues[0], &live_fill) == 0 && reads_as(live->system_words, FILL_WORD);
    check(
        "another context's queue still fills memory once the hung acquire's context is lost",
        filled && lost && lost_reason(live) == 0
    );

    close_session(&hung);
    check("closing the hung acquire's context gives its memory back", memory_returns_to(fd, before));
}

// The NON_STALL_INTERRUPT methods raise the copy vector, those raised while one is pending merging into it, and no
// handler pass they cost reports the queue's progress, so the wait ends only once the fill after them has run
static void test_interrupt_storm(struct session* s) {
    struct queue* q = &s->queues[0];
    volatile uint32_t* storm = s->command_words + (STORM_VA - COMMANDS_VA) / sizeof(uint32_t);
    for (uint32_t i = 0; i < STORM_METHODS; i++) {
        storm[i] = PUSH_IMMEDIATE | (NON_STALL_INTERRUPT >> 2);
    }

    struct command_list system_fill = fill_commands(s->copy_class, SYSTEM_VA, ~FILL_WORD);
    for (uint32_t i = 0; i < system_fill.count; i++) {
        storm[STORM_METHODS + i] = system_fill.words[i];
    }

    fill(s->system_words, FILL_WORD);

    uint64_t start_ns = monotonic_ns();
    int error = submit_words(s, q, STORM_VA, STORM_METHODS + system_fill.count, NULL);
    if (error == 0) {
        error = wait_for(s, q, GPU_SUBMIT_TIMEOUT_NS);
    }

    uint64_t elapsed_us = (monotonic_ns() - start_ns) / NS_PER_US;
    printf(
        "gputest: %u NON_STALL_INTERRUPT methods and a fill took %llu us\n",
        STORM_METHODS,
        (unsigned long long)elapsed_us
    );

    bool filled = error == 0 && reads_as(s->system_words, ~FILL_WORD);
    check("a user queue's wait for a storm of NON_STALL_INTERRUPT methods ends only after the fill behind it", filled);
}

static void test_session_end(struct session* s, struct gpu_memory before) {
    check("the context stays live through every check on its queues", lost_reason(s) == 0);

    struct gpu_queue_args second = {0};
    second.context = s->context;
    second.queue = s->queues[1].index;
    bool destroyed = gpu_call(s->fd, GPU_QUEUE_DESTROY, &second) == 0;

    close_session(s);
    bool returned = memory_returns_to(s->fd, before);
    check("destroying one queue and closing the context with the other gives their memory back", destroyed && returned);
}

static void test_copy_queues(int fd, const struct gpu_info* info) {
    struct gpu_memory before = read_memory(fd);
    struct session s = {0};
    s.fd = fd;
    s.copy_class = info->nvidia.class_copy;

    bool opened = open_session(&s, GPU_QUEUE_COPY, QUEUE_COUNT);
    check("a context with its buffers mapped opens two copy queues", opened);
    if (!opened) {
        close_session(&s);
        return;
    }

    test_fills(&s);
    test_shared_memory_fill(&s);
    test_copies(&s);
    test_held_copy(&s);
    test_waited_copies(&s);

    test_host_methods(&s);
    test_acquire(&s);
    test_interrupt_storm(&s);

    test_tlb_invalidate(fd, &s);
    test_physical_copy(fd, &s);
    test_faulting_fill(fd, &s);
    test_hung_acquire(fd, &s);

    test_session_end(&s, before);
}

static volatile uint32_t* video_word(struct session* s, uint64_t va) {
    return s->video_words + (va - VIDEO_VA) / sizeof(uint32_t);
}

static void write_words(volatile uint32_t* to, const uint32_t* words, size_t count) {
    for (size_t i = 0; i < count; i++) {
        to[i] = words[i];
    }
}

// Writes the grids' QMDs and programs to the video buffer, zero until then
static void write_grids(struct session* s) {
    write_qmd(video_word(s, STORE_QMD_VA), STORE_PROGRAM_VA, 1, 1, 0);
    write_qmd(video_word(s, EXIT_QMD_VA), EXIT_PROGRAM_VA, GRID_CTAS, GRID_THREADS, GRID_RELEASE_VA);
    write_words(video_word(s, STORE_PROGRAM_VA), STORE_PROGRAM, WORD_COUNT(STORE_PROGRAM));
    write_words(video_word(s, EXIT_PROGRAM_VA), EXIT_PROGRAM, WORD_COUNT(EXIT_PROGRAM));
    __sync_synchronize();
}

// Runs `list` on the session's graphics queue, waiting up to GRAPHICS_WAIT_NS. Returns 0 or the errno, with the
// microseconds until its point was reached in `elapsed_us`
static int run_graphics(struct session* s, const struct command_list* list, uint64_t* elapsed_us) {
    struct queue* q = &s->queues[0];
    uint64_t start_ns = monotonic_ns();
    int error = submit(s, q, list, NULL);
    if (error == 0) {
        error = wait_for(s, q, GRAPHICS_WAIT_NS);
    }

    *elapsed_us = (monotonic_ns() - start_ns) / NS_PER_US;
    return error;
}

static void test_graphics_fill(struct session* s) {
    struct command_list list = {0};
    list.subchannel = SUBCHANNEL_COPY;
    push_fill(&list, s->copy_class, SYSTEM_VA, FILL_WORD);

    bool filled = run(s, &s->queues[0], &list) == 0 && reads_as(s->system_words, FILL_WORD);
    check("a graphics queue fills memory with the copy class on its copy subchannel", filled);
}

// Returns the microseconds the clears took
static uint64_t test_clears(struct session* s, uint32_t class_3d) {
    struct command_list list = {0};
    push_clears(&list, class_3d);

    uint64_t elapsed_us = 0;
    bool cleared = run_graphics(s, &list, &elapsed_us) == 0 && reads_as(s->system_words, LAST_CLEAR_WORD);
    printf("gputest: %u clears of 1 MiB took %llu us\n", CLEAR_REPEATS + 1, (unsigned long long)elapsed_us);
    check("a graphics queue's 3D clears cover their target by the time its point is reached", cleared);
    return elapsed_us;
}

// Returns the microseconds the grid took
static uint64_t test_store_grid(struct session* s, uint32_t class_compute) {
    struct command_list list = {0};
    push_grid(&list, class_compute, STORE_QMD_VA);

    uint64_t elapsed_us = 0;
    bool stored = run_graphics(s, &list, &elapsed_us) == 0 && *video_word(s, STORED_VA) == STORED_WORD;
    printf("gputest: a one-thread grid took %llu us\n", (unsigned long long)elapsed_us);
    check("a graphics queue's compute grid stores its word by the time its point is reached", stored);
    return elapsed_us;
}

// Returns the microseconds the grid took
static uint64_t test_exit_grid(struct session* s, uint32_t class_compute) {
    struct command_list list = {0};
    push_grid(&list, class_compute, EXIT_QMD_VA);

    uint64_t elapsed_us = 0;
    bool released = run_graphics(s, &list, &elapsed_us) == 0 && *video_word(s, GRID_RELEASE_VA) == GRID_RELEASE_WORD;
    printf("gputest: a grid of 2^26 CTAs took %llu us\n", (unsigned long long)elapsed_us);
    check("a graphics queue's grid of 2^26 CTAs has written its release by the time its point is reached", released);
    return elapsed_us;
}

// Runs `list`, which clears the target and has a grid write `word` at `va`, printing its time as `what`'s. Returns
// whether the target and `word` both read back once the queue's point is reached
static bool ends_after_both(
    struct session* s,
    const struct command_list* list,
    uint64_t va,
    uint32_t word,
    const char* what
) {
    fill(s->system_words, FILL_WORD);
    *video_word(s, va) = 0;
    __sync_synchronize();

    uint64_t elapsed_us = 0;
    bool reached = run_graphics(s, list, &elapsed_us) == 0;
    printf("gputest: %s took %llu us\n", what, (unsigned long long)elapsed_us);

    bool cleared = reached && reads_as(s->system_words, LAST_CLEAR_WORD);
    return cleared && *video_word(s, va) == word;
}

// Each order sends first the work that outlasts the other alone, so where both pipes run at once a release waiting
// only for the pipe last used would be reached before the work sent first ended
static void test_both_orders(
    struct session* s,
    const struct gpu_info* info,
    uint64_t clears_us,
    uint64_t store_us,
    uint64_t grid_us
) {
    struct command_list clears_first = {0};
    push_clears(&clears_first, info->nvidia.class_3d);
    push_grid(&clears_first, info->nvidia.class_compute, STORE_QMD_VA);

    check("65 clears alone outlast a one-thread grid alone", clears_us > store_us);
    check(
        "a graphics queue's point after 3D clears and then a compute grid is reached once both ended",
        ends_after_both(s, &clears_first, STORED_VA, STORED_WORD, "65 clears and then a one-thread grid")
    );

    struct command_list grid_first = {0};
    push_grid(&grid_first, info->nvidia.class_compute, EXIT_QMD_VA);
    push_clears(&grid_first, info->nvidia.class_3d);

    check("a grid of 2^26 CTAs alone outlasts 65 clears alone", grid_us > clears_us);
    check(
        "a graphics queue's point after a compute grid and then 3D clears is reached once both ended",
        ends_after_both(s, &grid_first, GRID_RELEASE_VA, GRID_RELEASE_WORD, "a grid of 2^26 CTAs and then 65 clears")
    );
}

static void test_graphics_queue(int fd, const struct gpu_info* info) {
    struct gpu_memory before = read_memory(fd);
    struct session s = {0};
    s.fd = fd;
    s.copy_class = info->nvidia.class_copy;

    bool opened = open_session(&s, GPU_QUEUE_GRAPHICS, 1);
    check("a context with its buffers mapped opens a graphics queue", opened);
    if (!opened) {
        close_session(&s);
        return;
    }

    write_grids(&s);
    test_graphics_fill(&s);
    uint64_t clears_us = test_clears(&s, info->nvidia.class_3d);
    uint64_t store_us = test_store_grid(&s, info->nvidia.class_compute);
    uint64_t grid_us = test_exit_grid(&s, info->nvidia.class_compute);
    test_both_orders(&s, info, clears_us, store_us, grid_us);

    check("the context stays live through every check on its graphics queue", lost_reason(&s) == 0);
    close_session(&s);
    bool returned = memory_returns_to(fd, before);
    check("closing the graphics queue's context and its buffers gives their memory back", returned);
}

static void test_nvidia(int fd, const struct gpu_info* info) {
    print_facts(info);
    test_facts(info);
    test_free_memory(info, read_memory(fd));
    test_clock(fd);

    test_video_buffer(fd);
    test_system_buffer(fd);
    test_scanout_buffer(fd);
    test_window_refusal(fd);

    test_context_mappings(fd);
    test_shared_memory_import(fd);
    test_many_contexts(fd);
    test_copy_queues(fd, info);
    test_graphics_queue(fd, info);
}

// The GPU core publishes /dev/gpu0 once the driver's task registers the GPU after bring-up, so init can start stlxdm
// before the device exists
static int open_device_once_registered(void) {
    uint64_t deadline = monotonic_ns() + DEVICE_WAIT_NS;
    int fd = open("/dev/gpu0", O_RDWR);
    while (fd < 0 && monotonic_ns() < deadline) {
        usleep(POLL_US);
        fd = open("/dev/gpu0", O_RDWR);
    }

    return fd;
}

// The word a round fills, unique to the round and the context
static uint32_t round_word(uint32_t round, uint32_t context_index) {
    return FILL_WORD + round * REMAP_CONTEXTS + context_index;
}

// Returns the index of the first word unlike `value`, or BUFFER_WORDS when every word is `value`
static size_t first_unlike(const volatile uint32_t* words, uint32_t value) {
    size_t i = 0;
    while (i < BUFFER_WORDS && words[i] == value) {
        i++;
    }

    return i;
}

// Maps `s`'s own video buffer on even rounds and `other` on odd ones at REMAP_VA, fills it through its fixed address
// and copies REMAP_VA to system memory, so a stale mapping copies an older word. Returns the failed step or null
static const char* remap_fill_and_copy(
    struct session* s,
    int32_t other,
    uint32_t round,
    uint32_t word,
    int* error
) {
    bool is_odd = round % 2 != 0;
    int32_t video = is_odd ? other : s->video;
    uint64_t video_va = is_odd ? OTHER_VIDEO_VA : VIDEO_VA;

    struct queue* q = &s->queues[0];
    struct command_list buffer_fill = fill_commands(s->copy_class, video_va, word);
    struct command_list remapped_copy = copy_commands(s->copy_class, REMAP_VA, SYSTEM_VA, BUFFER_BYTES);

    *error = unmap(s->fd, s->context, REMAP_VA);
    if (*error != 0) {
        return "the unmap";
    }

    *error = map(s->fd, s->context, video, REMAP_VA, 0);
    if (*error != 0) {
        return "the map";
    }

    *error = run(s, q, &buffer_fill);
    if (*error != 0) {
        return "the fill";
    }

    *error = run(s, q, &remapped_copy);
    if (*error != 0) {
        return "the copy";
    }

    return NULL;
}

// Runs one round on `s`. A failure prints the failed step and its errno, or the first word read back unlike the fill
static bool round_passes(struct session* s, int32_t other, uint32_t round, uint32_t context_index) {
    uint32_t word = round_word(round, context_index);
    int error = 0;
    const char* failed_step = remap_fill_and_copy(s, other, round, word, &error);
    if (failed_step != NULL) {
        printf(
            "gputest: the remap check's round %u failed on context %u: %s failed: %s\n",
            round,
            context_index,
            failed_step,
            strerror(error)
        );
        return false;
    }

    size_t unlike = first_unlike(s->system_words, word);
    if (unlike < BUFFER_WORDS) {
        printf(
            "gputest: the remap check's round %u failed on context %u: word %zu read back 0x%x where the fill wrote "
            "0x%x, and the context reads lost %u\n",
            round,
            context_index,
            unlike,
            s->system_words[unlike],
            word,
            lost_reason(s)
        );
        return false;
    }

    return true;
}

// Runs rounds on each context in turn until REMAP_NS passed or one fails. Returns whether none failed, with how many
// rounds passed on every context in `rounds`
static bool remap_until_deadline(struct session* sessions, const int32_t* others, uint32_t* rounds) {
    uint64_t deadline = monotonic_ns() + REMAP_NS;
    *rounds = 0;
    while (monotonic_ns() < deadline) {
        for (uint32_t i = 0; i < REMAP_CONTEXTS; i++) {
            if (!round_passes(&sessions[i], others[i], *rounds, i)) {
                return false;
            }
        }

        (*rounds)++;
    }

    return true;
}

// Whether each context's own video buffer, which even rounds fill, still reads its last fill through its CPU mapping
// after the display mapped and unmapped gfxfb's buffers for the CPU while the rounds ran
static bool cpu_mappings_read_last_fills(struct session* sessions, uint32_t rounds) {
    uint32_t last_even_round = (rounds - 1) & ~1u;
    for (uint32_t i = 0; i < REMAP_CONTEXTS; i++) {
        if (!reads_as(sessions[i].video_words, round_word(last_even_round, i))) {
            return false;
        }
    }

    return true;
}

// Two contexts remap their video buffers while gfxfb's buffers are created and destroyed, so the display's scanout
// memory, its mappings for the CPU and its window bindings change while the contexts' mappings do
static void test_remaps(int fd, const struct gpu_info* info) {
    struct gpu_memory before = read_memory(fd);
    struct session sessions[REMAP_CONTEXTS] = {0};
    int32_t others[REMAP_CONTEXTS];

    bool opened = true;
    for (uint32_t i = 0; i < REMAP_CONTEXTS; i++) {
        sessions[i].fd = fd;
        sessions[i].copy_class = info->nvidia.class_copy;
        bool opens = open_session(&sessions[i], GPU_QUEUE_COPY, QUEUE_COUNT);

        create_buffer(fd, BUFFER_BYTES, GPU_PLACEMENT_VIDEO, 0, &others[i]);
        opened = opened && opens && map(fd, sessions[i].context, others[i], OTHER_VIDEO_VA, 0) == 0;
    }

    check("two contexts open copy queues, each with a second video buffer mapped", opened);
    bool cycling = opened && start_cycling_buffers();
    check("gputest in stlxdm's place starts creating gfxfb's buffers", cycling);

    uint64_t start_ns = monotonic_ns();
    uint64_t cycled_before = cycled_buffers();
    uint32_t rounds = 0;
    bool copied = cycling && remap_until_deadline(sessions, others, &rounds);
    uint64_t cycled = cycled_buffers() - cycled_before;
    uint64_t elapsed_ms = (monotonic_ns() - start_ns) / NS_PER_MS;

    bool stopped_cleanly = cycling && stop_cycling_buffers();
    bool cpu_reads_fills = copied && cpu_mappings_read_last_fills(sessions, rounds);
    for (uint32_t i = 0; i < REMAP_CONTEXTS; i++) {
        close_session(&sessions[i]);
        release(others[i], NULL);
    }

    // Printed after the wait for the display's last release, so a release that ends in time logs before these
    bool returned = memory_returns_to(fd, before);
    printf(
        "gputest: %u %s on each context took %llu ms, while gfxfb created and destroyed %llu %s\n",
        rounds,
        rounds == 1 ? "round" : "rounds",
        (unsigned long long)elapsed_ms,
        (unsigned long long)cycled,
        cycled == 1 ? "buffer" : "buffers"
    );

    check("every copy from a context's remapped address reads back the fill of the buffer mapped there", copied);
    check("each context's own video buffer reads its last fill through its CPU mapping", cpu_reads_fills);
    check("gfxfb created and destroyed buffers while the rounds ran, no call failing", cycled > 0 && stopped_cleanly);
    check("closing the contexts and /dev/gfxfb gives their memory back", returned);
}

// Run by gputest in stlxdm's place, since gfxfb fails any other open file's creations with EBUSY while stlxdm controls
// gfxfb's buffers
static void run_remap_check(void) {
    int fd = open_device_once_registered();
    if (fd < 0) {
        printf("gputest: /dev/gpu0 cannot be opened: %s\n", strerror(errno));
    }

    struct gpu_info info = {0};
    int error = fd >= 0 ? gpu_call(fd, GPU_GET_INFO, &info) : 0;
    if (error != 0) {
        printf("gputest: GPU_GET_INFO failed: %s\n", strerror(error));
    }

    bool is_nvidia = fd >= 0 && error == 0 && info.backend == GPU_BACKEND_NVIDIA;
    check("gputest in stlxdm's place opens /dev/gpu0 on an NVIDIA GPU once its driver registers it", is_nvidia);
    if (is_nvidia) {
        test_remaps(fd, &info);
    }

    if (fd >= 0) {
        close(fd);
    }
}

static int report(void) {
    printf("gputest: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc == 2 && strcmp(argv[1], HELD_CHILD_ARG) == 0) {
        return run_held_child();
    }

    // init starts stlxdm by this path with no argument, so gputest started that way stands in stlxdm's place
    if (argc == 1 && strcmp(argv[0], STLXDM_PATH) == 0) {
        run_remap_check();
        return report();
    }

    bool proto_only = argc == 2 && strcmp(argv[1], "proto") == 0;
    bool window_only = argc == 2 && strcmp(argv[1], "window") == 0;
    bool resize_only = argc == 2 && strcmp(argv[1], "resize") == 0;
    bool gpu_only = argc == 2 && strcmp(argv[1], "gpu") == 0;
    bool display_only = argc == 2 && strcmp(argv[1], "display") == 0;

    bool is_mode = proto_only || window_only || resize_only || gpu_only || display_only;
    if (argc > 1 && !is_mode) {
        printf("usage: gputest [proto | window | resize | gpu | display]\n");
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

    if (resize_only) {
        run_resize_check(fd);
        close(fd);
        return report();
    }

    struct gpu_info info = {0};
    int error = gpu_call(fd, GPU_GET_INFO, &info);
    if (error != 0) {
        printf("gputest: GPU_GET_INFO failed: %s\n", strerror(error));
        close(fd);
        return 1;
    }

    // The protocol and window checks alone, since the GPU checks' hangs could switch stlxdm to the CPU compositor
    if (window_only) {
        run_proto_checks();
        run_window_checks(fd, &info);
        close(fd);
        return report();
    }

    bool presents = display_only && start_presenting();
    if (display_only) {
        check("gputest holds gfxfb's buffers in stlxdm's place and presents on output 0", presents);
    }

    if (info.backend == GPU_BACKEND_VIRTIO) {
        test_virtio_gpu(fd, &info);
    } else {
        test_nvidia(fd, &info);
    }

    if (display_only) {
        bool presents_on_time = presents && stop_presenting();
        check("every present on output 0 latched within its deadline while the GPU checks ran", presents_on_time);
    }

    if (gpu_only || display_only) {
        close(fd);
        return report();
    }

    run_proto_checks();
    run_window_checks(fd, &info);
    close(fd);

    return report();
}
