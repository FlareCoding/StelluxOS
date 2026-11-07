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

    uint64_t main_bytes = (found.main.bytes + SUBCONTEXT_HEADER_BYTES - 1) & ~(SUBCONTEXT_HEADER_BYTES - 1);
    found.main.bytes = main_bytes + subcontext_count * SUBCONTEXT_HEADER_BYTES;
    layouts = found;

    return OK;
}

} // namespace drivers::nvidia
