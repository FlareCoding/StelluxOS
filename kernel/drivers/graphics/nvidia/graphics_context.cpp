// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/graphics_context.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"
#include "mm/heap.h"

namespace drivers::nvidia {

using layout_member = context_buffer_layout graphics_buffer_layouts::*;

// A buffer the driver allocates: its position in an engine's row of the sizes, and the member its layout goes to
struct buffer_slot {
    uint32_t      position;
    layout_member layout;
};

// The bytes of one subcontext's header, and the multiple a main buffer's state rounds up to before the headers
constexpr uint64_t SUBCONTEXT_HEADER_BYTES = 0x1000;

// The size the firmware gives a buffer the chip lacks, NVIDIA's NV_U32_MAX
constexpr uint32_t MISSING_BUFFER_SIZE = 0xFFFFFFFF;

// GR0's row in each reply
constexpr uint32_t GR0_ROW = 0;

constexpr buffer_slot BUFFER_SLOTS[] = {
    {rm::CONTEXT_BUFFER_MAIN, &graphics_buffer_layouts::main},
    {rm::CONTEXT_BUFFER_PATCH, &graphics_buffer_layouts::patch},
    {rm::CONTEXT_BUFFER_BUNDLE, &graphics_buffer_layouts::bundle},
    {rm::CONTEXT_BUFFER_PAGE_POOL, &graphics_buffer_layouts::page_pool},
    {rm::CONTEXT_BUFFER_ATTRIBUTE, &graphics_buffer_layouts::attribute},
    {rm::CONTEXT_BUFFER_RTV, &graphics_buffer_layouts::rtv},
    {rm::CONTEXT_BUFFER_FECS_EVENT, &graphics_buffer_layouts::fecs_event},
    {rm::CONTEXT_BUFFER_PRIV_ACCESS_MAP, &graphics_buffer_layouts::priv_access_map},
};

static uint64_t round_up(uint64_t value, uint64_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

static uint64_t power_of_two_at_least(uint64_t value) {
    uint64_t power = 1;
    while (power < value) {
        power <<= 1;
    }

    return power;
}

// Places `buffer` at the first multiple of `alignment` and of 4 KiB from `address`, returning the address past it
static uint64_t place(uint64_t address, uint64_t bytes, uint64_t alignment, context_buffer& buffer) {
    uint64_t multiple = alignment > SMALL_PAGE_BYTES ? alignment : SMALL_PAGE_BYTES;
    buffer.gpu_address = round_up(address, multiple);
    return buffer.gpu_address + round_up(bytes, SMALL_PAGE_BYTES);
}

// Maps `buffer` at its address for the GPU's privileged accesses alone, writable unless `read_only`
static int32_t map_privileged(context_page_tables& tables, const context_buffer& buffer, bool read_only) {
    memory_run run = {.address = buffer.range.offset, .bytes = buffer.range.size};
    page_mapping mapping = {
        .aperture = page_aperture::video_memory,
        .kind = 0,
        .read_only = read_only,
        .privileged = true,
    };

    return tables.map(buffer.gpu_address, &run, 1, mapping);
}

// Adds `buffer`'s entry, naming its video memory for the firmware to initialize if `initializes` and its address if
// `is_mapped`
static void add_entry(
    rm::promote_context_params& params,
    uint16_t id,
    const context_buffer& buffer,
    bool initializes,
    bool is_mapped
) {
    rm::promote_context_entry& entry = params.entries[params.entry_count];
    entry.buffer_id = id;

    if (initializes) {
        entry.phys_address = buffer.range.offset;
        entry.size = buffer.range.size;
        entry.phys_attributes = rm::INITIALIZED_IN_VIDEO_MEMORY;
        entry.initializes = 1;
        entry.is_unmapped = is_mapped ? 0 : 1;
    }

    if (is_mapped) {
        entry.gpu_address = buffer.gpu_address;
    }

    params.entry_count++;
}

// Reads GR0's subcontext count from the facts the reply filled, refusing a fact out of place or a count of 0
static int32_t find_subcontext_count(const rm::graphics_info_params& facts, uint32_t& count) {
    const rm::graphics_info& fact = facts.engines[GR0_ROW][rm::GRAPHICS_INFO_SUBCONTEXTS];
    if (fact.index != rm::GRAPHICS_INFO_SUBCONTEXTS) {
        log::error(
            "nvidia: the GSP firmware's graphics facts hold fact 0x%X at the subcontext count's position 0x%X",
            fact.index,
            rm::GRAPHICS_INFO_SUBCONTEXTS
        );
        return ERR_BAD_MESSAGE;
    }

    if (fact.data == 0) {
        log::error("nvidia: the GSP firmware's graphics facts give GR0 no subcontext");
        return ERR_BAD_MESSAGE;
    }

    count = fact.data;
    return OK;
}

static int32_t read_subcontext_count(firmware& fw, uint32_t& count) {
    uint32_t facts_bytes = sizeof(rm::graphics_info_params);
    auto* facts = static_cast<rm::graphics_info_params*>(heap::uzalloc(facts_bytes));
    if (facts == nullptr) {
        log::error("nvidia: allocating %u bytes for the GSP firmware's graphics facts failed", facts_bytes);
        return ERR_NO_MEMORY;
    }

    int32_t rc = fw.internal_control(rm::CONTROL_GET_GRAPHICS_INFO, facts, facts_bytes);
    if (rc == OK) {
        rc = find_subcontext_count(*facts, count);
    }

    heap::ufree(facts);

    return rc;
}

// Reads every buffer the driver allocates from GR0's row of the sizes the reply filled, refusing one sized 0 or missing
static int32_t find_layouts(const rm::context_buffers_params& sizes, graphics_buffer_layouts& layouts) {
    for (const buffer_slot& slot : BUFFER_SLOTS) {
        const rm::context_buffer_info& info = sizes.engines[GR0_ROW][slot.position];
        bool is_sized = info.size != 0 && info.size != MISSING_BUFFER_SIZE;
        if (!is_sized) {
            log::error(
                "nvidia: the GSP firmware sizes GR0's context buffer 0x%X as 0x%X bytes, which the driver cannot "
                "allocate",
                slot.position,
                info.size
            );
            return ERR_BAD_MESSAGE;
        }

        layouts.*slot.layout = {.bytes = info.size, .alignment = info.alignment};
    }

    return OK;
}

int32_t read_graphics_buffer_layouts(firmware& fw, graphics_buffer_layouts& layouts) {
    uint32_t subcontext_count = 0;
    int32_t rc = read_subcontext_count(fw, subcontext_count);
    if (rc != OK) {
        return rc;
    }

    uint32_t sizes_bytes = sizeof(rm::context_buffers_params);
    auto* sizes = static_cast<rm::context_buffers_params*>(heap::uzalloc(sizes_bytes));
    if (sizes == nullptr) {
        log::error("nvidia: allocating %u bytes for the GSP firmware's context buffer sizes failed", sizes_bytes);
        return ERR_NO_MEMORY;
    }

    graphics_buffer_layouts found = {};
    rc = fw.internal_control(rm::CONTROL_GET_CONTEXT_BUFFERS, sizes, sizes_bytes);
    if (rc == OK) {
        rc = find_layouts(*sizes, found);
    }

    heap::ufree(sizes);

    if (rc != OK) {
        return rc;
    }

    uint64_t main_bytes = round_up(found.main.bytes, SUBCONTEXT_HEADER_BYTES);
    found.main.bytes = main_bytes + subcontext_count * SUBCONTEXT_HEADER_BYTES;
    layouts = found;

    return OK;
}

uint64_t place_global_buffers(const graphics_buffer_layouts& layouts, uint64_t base, global_context_buffers& global) {
    uint64_t attribute_alignment = power_of_two_at_least(layouts.attribute.bytes);
    uint64_t next = place(base, layouts.attribute.bytes, attribute_alignment, global.attribute);

    next = place(next, layouts.bundle.bytes, layouts.bundle.alignment, global.bundle);
    next = place(next, layouts.page_pool.bytes, layouts.page_pool.alignment, global.page_pool);
    next = place(next, layouts.rtv.bytes, layouts.rtv.alignment, global.rtv);
    next = place(next, layouts.fecs_event.bytes, layouts.fecs_event.alignment, global.fecs_event);
    next = place(next, layouts.priv_access_map.bytes, layouts.priv_access_map.alignment, global.priv_access_map);

    global.unrestricted_priv_access_map.gpu_address = 0;

    return next;
}

void place_own_buffers(
    const graphics_buffer_layouts& layouts,
    uint64_t start,
    uint32_t slot,
    channel_context_buffers& own
) {
    uint64_t alignment = layouts.main.alignment > SMALL_PAGE_BYTES ? layouts.main.alignment : SMALL_PAGE_BYTES;
    alignment = layouts.patch.alignment > alignment ? layouts.patch.alignment : alignment;

    // Slot 0's span from a multiple of both alignments, rounded up to one, keeps every later slot aligned like slot 0
    uint64_t first = round_up(start, alignment);
    uint64_t past_main = place(first, layouts.main.bytes, layouts.main.alignment, own.main);
    uint64_t span = place(past_main, layouts.patch.bytes, layouts.patch.alignment, own.patch) - first;

    uint64_t slot_start = first + slot * round_up(span, alignment);
    uint64_t next = place(slot_start, layouts.main.bytes, layouts.main.alignment, own.main);
    place(next, layouts.patch.bytes, layouts.patch.alignment, own.patch);
}

int32_t map_global_buffers(context_page_tables& tables, const global_context_buffers& global) {
    const context_buffer* writable[] = {
        &global.bundle,
        &global.page_pool,
        &global.attribute,
        &global.rtv,
        &global.fecs_event,
    };

    for (const context_buffer* buffer : writable) {
        int32_t rc = map_privileged(tables, *buffer, false);
        if (rc != OK) {
            return rc;
        }
    }

    return map_privileged(tables, global.priv_access_map, true);
}

int32_t map_own_buffers(context_page_tables& tables, const channel_context_buffers& own) {
    int32_t rc = map_privileged(tables, own.main, false);
    if (rc != OK) {
        return rc;
    }

    return map_privileged(tables, own.patch, false);
}

int32_t promote_graphics_context(
    firmware& fw,
    rm::handle subdevice,
    rm::handle channel,
    const global_context_buffers& global,
    const channel_context_buffers& own,
    bool initializes_global
) {
    rm::promote_context_params params = {};
    params.engine_type = rm::ENGINE_TYPE_GRAPHICS;
    params.channel_client = fw.client();
    params.channel = channel;

    add_entry(params, rm::PROMOTED_MAIN, own.main, true, true);
    add_entry(params, rm::PROMOTED_PATCH, own.patch, true, true);

    add_entry(params, rm::PROMOTED_BUNDLE, global.bundle, false, true);
    add_entry(params, rm::PROMOTED_PAGE_POOL, global.page_pool, false, true);
    add_entry(params, rm::PROMOTED_ATTRIBUTE, global.attribute, false, true);
    add_entry(params, rm::PROMOTED_RTV, global.rtv, false, true);

    add_entry(params, rm::PROMOTED_FECS_EVENT, global.fecs_event, initializes_global, true);
    add_entry(params, rm::PROMOTED_PRIV_ACCESS_MAP, global.priv_access_map, initializes_global, true);
    if (initializes_global) {
        add_entry(params, rm::PROMOTED_UNRESTRICTED_PRIV_ACCESS_MAP, global.unrestricted_priv_access_map, true, false);
    }

    int32_t rc = fw.control(subdevice, rm::CONTROL_PROMOTE_CONTEXT, &params, sizeof(params));
    if (rc != OK) {
        log::error("nvidia: promoting the context buffers of graphics channel object 0x%X failed: %d", channel, rc);
    }

    return rc;
}

} // namespace drivers::nvidia
