// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/instance_memory.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"
#include "hw/mmio.h"

namespace drivers::nvidia {

// A hash table entry, at the memory's start: the context DMA's handle, then a word holding the client's low 14 bits,
// the record's offset in 32-byte units from bit 14 and the channel from bit 25
constexpr uint64_t ENTRY_BYTES         = 8;
constexpr uint32_t ENTRY_CLIENT_MASK   = 0x3FFF;
constexpr uint32_t ENTRY_RECORD_SHIFT  = 14;
constexpr uint32_t ENTRY_CHANNEL_SHIFT = 25;

// A record, in 32-byte slots from 0x2020, past the hash table and NVIDIA's NULL instance: its target, access and kind,
// then the first and last 256-byte unit of its memory, each as a low and a high word
constexpr uint64_t FIRST_RECORD_OFFSET = 0x2020;
constexpr uint64_t RECORD_BYTES        = 32;
constexpr uint32_t RECORD_UNIT_SHIFT   = 8;
constexpr uint64_t RECORD_UNIT_BYTES   = 0x100;

// A record's targets, video memory and pages read with snooping on, and its read and write access. The pitch kind
// leaves bit 20 clear
constexpr uint32_t RECORD_TARGET_VIDEO_MEMORY  = 1;
constexpr uint32_t RECORD_TARGET_SNOOPED_PAGES = 3;
constexpr uint32_t RECORD_READ_AND_WRITE       = 1u << 2;

// A record's high words hold 7 bits of a 256-byte unit, so its memory ends within the first 128 TiB
constexpr uint64_t RECORD_ADDRESS_LIMIT = 1ull << 47;

// The core's and the windows' channels take numbers 0 to 64 and the cursors' 73 to 80, so 65 to 72 name no channel
constexpr uint32_t CORE_AND_WINDOW_CHANNEL_NUMBERS = 65;
constexpr uint32_t FIRST_CURSOR_CHANNEL            = 73;

static_assert(INSTANCE_MEMORY_HASH_ENTRIES * ENTRY_BYTES + RECORD_BYTES == FIRST_RECORD_OFFSET, "NVIDIA's layout");
static_assert(FIRST_RECORD_OFFSET + INSTANCE_MEMORY_RECORDS * RECORD_BYTES == INSTANCE_MEMORY_BYTES, "NVIDIA's layout");
static_assert(INSTANCE_MEMORY_BYTES / RECORD_BYTES <= 1u << 11, "every record's offset fits an entry's 11 bits");
static_assert(DISPLAY_CHANNEL_NUMBERS <= 1u << 7, "a channel fits an entry's 7 bits, and a record's users a byte");
static_assert(
    INSTANCE_MEMORY_HASH_ENTRIES < INSTANCE_MEMORY_RECORDS,
    "a record in use has an entry, so a free entry leaves a free record"
);

// NVIDIA's hash of a binding, below 1024, where the search for a free entry starts: the handle's bits 9:0, 19:10 and
// 29:20, {client 7:0, handle 31:30}, {channel 3:0, client 13:8} and {channel 6:4}, XORed
static uint32_t hash_binding(rm::handle client, rm::handle object, uint32_t hardware_channel) {
    uint32_t object_fields = (object & 0x3FF) ^ ((object >> 10) & 0x3FF) ^ ((object >> 20) & 0x3FF);
    uint32_t client_and_object_top = ((client & 0xFF) << 2) | (object >> 30);
    uint32_t channel_and_client_high = ((hardware_channel & 0xF) << 6) | ((client >> 8) & 0x3F);
    uint32_t channel_high = (hardware_channel >> 4) & 0x7;

    return object_fields ^ client_and_object_top ^ channel_and_client_high ^ channel_high;
}

// Whether `hardware_channel` is the number of one of the display engine's channels
static bool names_channel(uint32_t hardware_channel) {
    bool is_core_or_window = hardware_channel < CORE_AND_WINDOW_CHANNEL_NUMBERS;
    bool is_cursor = hardware_channel >= FIRST_CURSOR_CHANNEL && hardware_channel < DISPLAY_CHANNEL_NUMBERS;
    return is_core_or_window || is_cursor;
}

// `OK` when a record can name `memory`, else `ERR_INVALID_ARG`, logging why for `object`'s binding to
// `hardware_channel`
static int32_t check_memory(rm::handle object, uint32_t hardware_channel, const context_dma_memory& memory) {
    const char* space = memory.in_video_memory ? "in video memory" : "in system memory";

    if (memory.address % RECORD_UNIT_BYTES != 0) {
        log::error(
            "nvidia: binding context DMA 0x%X to display channel %u refused: its memory at 0x%lX %s does not start "
            "on a 256-byte boundary",
            object,
            hardware_channel,
            memory.address,
            space
        );
        return ERR_INVALID_ARG;
    }

    if (memory.bytes == 0) {
        log::error(
            "nvidia: binding context DMA 0x%X to display channel %u refused: its memory at 0x%lX %s holds 0 bytes",
            object,
            hardware_channel,
            memory.address,
            space
        );
        return ERR_INVALID_ARG;
    }

    if (!lies_inside(memory.address, memory.bytes, RECORD_ADDRESS_LIMIT)) {
        log::error(
            "nvidia: binding context DMA 0x%X to display channel %u refused: its 0x%lX bytes at 0x%lX %s end past "
            "128 TiB, the most a record can name",
            object,
            hardware_channel,
            memory.bytes,
            memory.address,
            space
        );
        return ERR_INVALID_ARG;
    }

    return OK;
}

// Where `record`'s words start in the instance memory
static uint64_t record_offset(uint32_t record) {
    return FIRST_RECORD_OFFSET + record * RECORD_BYTES;
}

// Writes `record`'s five words through `mapping` as NVIDIA's driver writes a context DMA's
static void write_record(const cpu_mapping& mapping, uint32_t record, const context_dma_memory& memory) {
    uint32_t target = memory.in_video_memory ? RECORD_TARGET_VIDEO_MEMORY : RECORD_TARGET_SNOOPED_PAGES;
    uint64_t first_unit = memory.address >> RECORD_UNIT_SHIFT;
    uint64_t last_unit = (memory.address + memory.bytes - 1) >> RECORD_UNIT_SHIFT;
    uintptr_t words = mapping.virt + record_offset(record);

    mmio::write32(words, target | RECORD_READ_AND_WRITE);
    mmio::write32(words + 4, static_cast<uint32_t>(first_unit));
    mmio::write32(words + 8, static_cast<uint32_t>(first_unit >> 32));
    mmio::write32(words + 12, static_cast<uint32_t>(last_unit));
    mmio::write32(words + 16, static_cast<uint32_t>(last_unit >> 32));
}

// Writes `entry`'s two words through `mapping`
static void write_entry(const cpu_mapping& mapping, uint32_t entry, rm::handle object, uint32_t binding_word) {
    uintptr_t words = mapping.virt + entry * ENTRY_BYTES;

    mmio::write32(words, object);
    mmio::write32(words + 4, binding_word);
}

// An entry's second word, naming the binding's client, record and channel
static uint32_t entry_binding_word(rm::handle client, uint32_t record, uint32_t hardware_channel) {
    uint32_t client_bits = client & ENTRY_CLIENT_MASK;
    uint32_t record_bits = static_cast<uint32_t>(record_offset(record) / RECORD_BYTES) << ENTRY_RECORD_SHIFT;
    uint32_t channel_bits = hardware_channel << ENTRY_CHANNEL_SHIFT;

    return client_bits | record_bits | channel_bits;
}

instance_memory::instance_memory(video_memory& memory, firmware& fw)
    : m_memory(memory)
    , m_fw(fw)
    , m_range{}
    , m_bindings{}
    , m_record_users{} {}

int32_t instance_memory::set_up() {
    vram_range allocated = {};
    int32_t rc = m_memory.allocate(INSTANCE_MEMORY_BYTES, INSTANCE_MEMORY_ALIGNMENT, vram_use::general, allocated);
    if (rc != OK) {
        log::error(
            "nvidia: allocating 0x%lX bytes of video memory at alignment 0x%lX for the display's instance memory "
            "failed: %d",
            INSTANCE_MEMORY_BYTES,
            INSTANCE_MEMORY_ALIGNMENT,
            rc
        );
        return rc;
    }

    cpu_mapping mapping = {};
    rc = m_memory.map_for_cpu(allocated, mapping);
    if (rc != OK) {
        log::error(
            "nvidia: mapping the display's instance memory at 0x%lX in video memory for the CPU to clear it failed: "
            "%d, so the memory is released",
            allocated.offset,
            rc
        );
        m_memory.release(allocated);
        return rc;
    }

    for (uint64_t offset = 0; offset < INSTANCE_MEMORY_BYTES; offset += sizeof(uint64_t)) {
        mmio::write64(mapping.virt + offset, 0);
    }

    m_memory.unmap_for_cpu(allocated);
    m_range = allocated;

    rm::display_instance_memory_params params = {};
    params.address = allocated.offset;
    params.bytes = INSTANCE_MEMORY_BYTES;
    params.address_space = rm::ADDRESS_SPACE_VIDEO_MEMORY;
    params.cpu_caching = rm::CPU_CACHING_WRITE_COMBINED;

    rc = m_fw.internal_control(rm::CONTROL_WRITE_DISPLAY_INSTANCE_MEMORY, &params, sizeof(params));
    if (rc != OK) {
        log::error(
            "nvidia: telling the GSP's firmware where the display's instance memory lies, at 0x%lX in video memory, "
            "failed: %d, so the memory stays allocated",
            allocated.offset,
            rc
        );
        return rc;
    }

    return OK;
}

const vram_range& instance_memory::range() const {
    return m_range;
}

int32_t instance_memory::bind(rm::handle object, uint32_t hardware_channel, const context_dma_memory& memory) {
    if (object == 0) {
        log::error(
            "nvidia: binding a context DMA to display channel %u refused: handle 0 names no object",
            hardware_channel
        );
        return ERR_INVALID_ARG;
    }

    if (!names_channel(hardware_channel)) {
        log::error(
            "nvidia: binding context DMA 0x%X to display channel %u refused: the display's channels are "
            "numbered 0 to %u and %u to %u",
            object,
            hardware_channel,
            CORE_AND_WINDOW_CHANNEL_NUMBERS - 1,
            FIRST_CURSOR_CHANNEL,
            DISPLAY_CHANNEL_NUMBERS - 1
        );
        return ERR_INVALID_ARG;
    }

    int32_t rc = check_memory(object, hardware_channel, memory);
    if (rc != OK) {
        return rc;
    }

    if (m_range.size == 0) {
        log::error(
            "nvidia: binding context DMA 0x%X to display channel %u refused: the display's instance memory "
            "is not set up",
            object,
            hardware_channel
        );
        return ERR_BAD_STATE;
    }

    rm::handle client = m_fw.client();
    if (client == 0) {
        log::error(
            "nvidia: binding context DMA 0x%X to display channel %u refused: the driver has no client",
            object,
            hardware_channel
        );
        return ERR_BAD_STATE;
    }

    if (find_binding(object, hardware_channel) != INSTANCE_MEMORY_HASH_ENTRIES) {
        log::error(
            "nvidia: binding context DMA 0x%X to display channel %u refused: the context DMA is bound to "
            "that channel already",
            object,
            hardware_channel
        );
        return ERR_BAD_STATE;
    }

    uint32_t entry = find_free_entry(hash_binding(client, object, hardware_channel));
    if (entry == INSTANCE_MEMORY_HASH_ENTRIES) {
        log::error(
            "nvidia: binding context DMA 0x%X to display channel %u failed: the display's hash table is full",
            object,
            hardware_channel
        );
        return ERR_NO_MEMORY;
    }

    uint32_t record = find_record(object);
    bool is_first_binding = record == INSTANCE_MEMORY_RECORDS;
    if (is_first_binding) {
        record = find_free_record();
    }

    cpu_mapping mapping = {};
    rc = m_memory.map_for_cpu(m_range, mapping);
    if (rc != OK) {
        log::error(
            "nvidia: mapping the display's instance memory for the CPU to bind context DMA 0x%X to display "
            "channel %u failed: %d",
            object,
            hardware_channel,
            rc
        );
        return rc;
    }

    if (is_first_binding) {
        write_record(mapping, record, memory);
    }

    write_entry(mapping, entry, object, entry_binding_word(client, record, hardware_channel));
    m_memory.unmap_for_cpu(m_range);

    m_bindings[entry] = {object, static_cast<uint16_t>(record), static_cast<uint8_t>(hardware_channel)};
    m_record_users[record]++;

    return OK;
}

int32_t instance_memory::unbind(rm::handle object, uint32_t hardware_channel) {
    uint32_t entry = find_binding(object, hardware_channel);
    if (entry == INSTANCE_MEMORY_HASH_ENTRIES) {
        log::error(
            "nvidia: unbinding context DMA 0x%X from display channel %u refused: the context DMA is not bound to "
            "that channel",
            object,
            hardware_channel
        );
        return ERR_BAD_STATE;
    }

    cpu_mapping mapping = {};
    int32_t rc = m_memory.map_for_cpu(m_range, mapping);
    if (rc != OK) {
        log::error(
            "nvidia: mapping the display's instance memory for the CPU to unbind context DMA 0x%X from display "
            "channel %u failed: %d, so the binding is kept",
            object,
            hardware_channel,
            rc
        );
        return rc;
    }

    write_entry(mapping, entry, 0, 0);
    m_memory.unmap_for_cpu(m_range);

    m_record_users[m_bindings[entry].record]--;
    m_bindings[entry] = {};

    return OK;
}

uint32_t instance_memory::find_binding(rm::handle object, uint32_t hardware_channel) const {
    // Every free entry holds handle 0, which names no object
    if (object == 0) {
        return INSTANCE_MEMORY_HASH_ENTRIES;
    }

    for (uint32_t entry = 0; entry < INSTANCE_MEMORY_HASH_ENTRIES; entry++) {
        const binding& held = m_bindings[entry];
        if (held.object == object && held.hardware_channel == hardware_channel) {
            return entry;
        }
    }

    return INSTANCE_MEMORY_HASH_ENTRIES;
}

uint32_t instance_memory::find_free_entry(uint32_t hash) const {
    for (uint32_t step = 0; step < INSTANCE_MEMORY_HASH_ENTRIES; step++) {
        uint32_t entry = (hash + step) % INSTANCE_MEMORY_HASH_ENTRIES;
        if (m_bindings[entry].object == 0) {
            return entry;
        }
    }

    return INSTANCE_MEMORY_HASH_ENTRIES;
}

uint32_t instance_memory::find_record(rm::handle object) const {
    for (const binding& held : m_bindings) {
        if (held.object == object) {
            return held.record;
        }
    }

    return INSTANCE_MEMORY_RECORDS;
}

uint32_t instance_memory::find_free_record() const {
    uint32_t record = 0;
    while (m_record_users[record] != 0) {
        record++;
    }

    return record;
}

} // namespace drivers::nvidia
