// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/channel_methods.h"

namespace drivers::nvidia {

// NVIDIA's NVC56F_DMA_SEC_OP_INC_METHOD, a push buffer header whose values go to consecutive methods, with the value
// count from bit 16 and the subchannel from bit 13
constexpr uint32_t PUSH_HEADER_INCREASING       = 1u << 29;
constexpr uint32_t PUSH_HEADER_COUNT_SHIFT      = 16;
constexpr uint32_t PUSH_HEADER_SUBCHANNEL_SHIFT = 13;

// NVIDIA's RM_SUBCHANNEL, the subchannel NVIDIA's own copy channel sends host and copy methods on
constexpr uint32_t SUBCHANNEL = 0;

// NVIDIA's NVC56F host methods
constexpr uint32_t SET_OBJECT          = 0x0000;
constexpr uint32_t NON_STALL_INTERRUPT = 0x0020;
constexpr uint32_t MEM_OP_A            = 0x0028;
constexpr uint32_t SEM_ADDR_LO         = 0x005C;
constexpr uint32_t WAIT_FOR_IDLE       = 0x0078;

// NVIDIA's NVC56F_MEM_OP_D_OPERATION_MEMBAR, in MEM_OP_D's bits 31:27, with MEM_OP_C's SYS_MEMBAR type 0
constexpr uint32_t MEM_OP_D_MEMBAR = 0x5u << 27;

// NVIDIA's NVC56F_SEM_EXECUTE_OPERATION_RELEASE, with RELEASE_WFI_DIS and PAYLOAD_SIZE_32BIT
constexpr uint32_t SEM_EXECUTE_RELEASE = 0x1;

// NVIDIA's NVC7B5 copy methods
constexpr uint32_t LAUNCH_DMA           = 0x0300;
constexpr uint32_t SET_DST_PHYS_MODE    = 0x0264;
constexpr uint32_t OFFSET_OUT_UPPER     = 0x0408;
constexpr uint32_t LINE_LENGTH_IN       = 0x0418;
constexpr uint32_t SET_REMAP_CONST_A    = 0x0700;
constexpr uint32_t SET_REMAP_COMPONENTS = 0x0708;

// NVIDIA's NVC7B5_SET_DST_PHYS_MODE_TARGET_LOCAL_FB, physical addresses in video memory
constexpr uint32_t PHYS_MODE_VIDEO_MEMORY = 0x0;

// NVIDIA's SET_REMAP_COMPONENTS fields: DST_X CONST_A, COMPONENT_SIZE FOUR, one destination component
constexpr uint32_t REMAP_FILL_ELEMENTS = 0x4u | (0x3u << 16);

// NVIDIA's LAUNCH_DMA fields: NON_PIPELINED, FLUSH_ENABLE, SRC and DST_MEMORY_LAYOUT PITCH, REMAP_ENABLE, DST_TYPE
// PHYSICAL and DISABLE_PLC, without a semaphore or interrupt of the copy engine's own
constexpr uint32_t LAUNCH_FILL = 0x2u | (1u << 2) | (1u << 7) | (1u << 8) | (1u << 10) | (1u << 13) | (1u << 26);

// NVIDIA's NVC56F_GP_ENTRY1_LENGTH, from bit 10, and NVC56F_GP_ENTRY1_SYNC_WAIT
constexpr uint32_t GPFIFO_LENGTH_SHIFT = 10;
constexpr uint32_t GPFIFO_SYNC_WAIT    = 1u << 31;

// NVIDIA's NV_CTRL_VF_DOORBELL_RUNLIST_ID, from bit 16
constexpr uint32_t DOORBELL_RUNLIST_SHIFT = 16;

// NVIDIA's NVOS04_FLAGS fields of a channel's allocation
constexpr uint32_t ALLOCATION_PRIVILEGED           = 1u << 5;
constexpr uint32_t ALLOCATION_DELAY_SCHEDULING     = 1u << 6;
constexpr uint32_t ALLOCATION_DENY_PHYSICAL_COPIES = 1u << 7;
constexpr uint32_t ALLOCATION_USERD_INDEX_SHIFT    = 8;
constexpr uint32_t ALLOCATION_USERD_PAGE_SHIFT     = 12;
constexpr uint32_t ALLOCATION_USERD_PAGE_FIXED     = 1u << 21;
constexpr uint32_t ALLOCATION_DENY_PRIVILEGED_OPS  = 1u << 22;

// NVIDIA's channels per USERD page, a 4 KiB page of 512-byte USERDs
constexpr uint32_t CHANNELS_PER_USERD_PAGE = 8;

static uint32_t method_header(uint32_t method, uint32_t count) {
    uint32_t subchannel = SUBCHANNEL << PUSH_HEADER_SUBCHANNEL_SHIFT;
    return PUSH_HEADER_INCREASING | (count << PUSH_HEADER_COUNT_SHIFT) | subchannel | (method >> 2);
}

static uint32_t* write_method(uint32_t* words, uint32_t method, uint32_t value) {
    words[0] = method_header(method, 1);
    words[1] = value;
    return words + 2;
}

static uint32_t* write_memory_barrier(uint32_t* words) {
    words[0] = method_header(MEM_OP_A, 4);
    words[1] = 0;
    words[2] = 0;
    words[3] = 0;
    words[4] = MEM_OP_D_MEMBAR;
    return words + 5;
}

static uint32_t low_word(uint64_t value) {
    return static_cast<uint32_t>(value);
}

static uint32_t high_word(uint64_t value) {
    return static_cast<uint32_t>(value >> 32);
}

gpfifo_entry make_gpfifo_entry(uint64_t gpu_address, uint32_t word_count, gpfifo_fetch fetch) {
    uint32_t sync = 0;
    switch (fetch) {
        case gpfifo_fetch::ahead:
            break;
        case gpfifo_fetch::after_earlier:
            sync = GPFIFO_SYNC_WAIT;
            break;
    }

    gpfifo_entry entry = {};
    entry.words[0] = low_word(gpu_address) & ~3u;
    entry.words[1] = (high_word(gpu_address) & 0xFF) | (word_count << GPFIFO_LENGTH_SHIFT) | sync;
    return entry;
}

void write_fill(uint32_t* words, uint32_t copy_class, uint64_t offset, uint64_t bytes, uint32_t value) {
    words = write_method(words, SET_OBJECT, copy_class);
    words = write_method(words, SET_REMAP_CONST_A, value);
    words = write_method(words, SET_REMAP_COMPONENTS, REMAP_FILL_ELEMENTS);
    words = write_method(words, SET_DST_PHYS_MODE, PHYS_MODE_VIDEO_MEMORY);

    words[0] = method_header(OFFSET_OUT_UPPER, 2);
    words[1] = high_word(offset);
    words[2] = low_word(offset);
    words += 3;

    words = write_method(words, LINE_LENGTH_IN, static_cast<uint32_t>(bytes / 4));
    write_method(words, LAUNCH_DMA, LAUNCH_FILL);
}

void write_release(uint32_t* words, uint64_t semaphore_address, uint32_t payload) {
    words = write_method(words, WAIT_FOR_IDLE, 0);
    words = write_memory_barrier(words);

    words[0] = method_header(SEM_ADDR_LO, 5);
    words[1] = low_word(semaphore_address) & ~3u;
    words[2] = high_word(semaphore_address) & 0xFF;
    words[3] = payload;
    words[4] = 0;
    words[5] = SEM_EXECUTE_RELEASE;

    words = write_memory_barrier(words + 6);
    write_method(words, NON_STALL_INTERRUPT, 0);
}

uint32_t doorbell_token(uint32_t runlist, uint32_t channel_id) {
    return (runlist << DOORBELL_RUNLIST_SHIFT) | channel_id;
}

uint32_t channel_allocation_flags(uint32_t channel_id, bool is_privileged) {
    uint32_t denied = ALLOCATION_DENY_PHYSICAL_COPIES | ALLOCATION_DENY_PRIVILEGED_OPS;
    uint32_t privilege = is_privileged ? ALLOCATION_PRIVILEGED : denied;
    uint32_t index = channel_id % CHANNELS_PER_USERD_PAGE;
    uint32_t page = channel_id / CHANNELS_PER_USERD_PAGE;
    return privilege | ALLOCATION_DELAY_SCHEDULING | (index << ALLOCATION_USERD_INDEX_SHIFT)
        | (page << ALLOCATION_USERD_PAGE_SHIFT) | ALLOCATION_USERD_PAGE_FIXED;
}

} // namespace drivers::nvidia
