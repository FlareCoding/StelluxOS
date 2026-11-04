#include "drivers/graphics/nvidia/engine_table.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"

namespace drivers::nvidia {

static bool is_copy_engine(uint32_t type) {
    return type >= rm::ENGINE_TYPE_FIRST_COPY && type <= rm::ENGINE_TYPE_LAST_COPY;
}

// Appends a page's entries to the `count` engines `engines` holds
static int32_t keep_page(const rm::device_info_table_params& page, engine_info* engines, uint32_t& count) {
    if (page.entry_count > rm::DEVICE_INFO_PAGE_ENTRIES) {
        log::error(
            "nvidia: the GSP firmware's device info table page at entry %u counts %u entries, more than the %u it "
            "has room for",
            page.first_entry,
            page.entry_count,
            rm::DEVICE_INFO_PAGE_ENTRIES
        );
        return ERR_BAD_MESSAGE;
    }

    for (uint32_t i = 0; i < page.entry_count; i++) {
        const rm::device_info_entry& entry = page.entries[i];
        engines[count].type = entry.data[rm::DEVICE_INFO_ENGINE_TYPE];
        engines[count].runlist = entry.data[rm::DEVICE_INFO_RUNLIST];
        count++;
    }

    return OK;
}

int32_t engine_table::read(firmware& fw, rm::handle subdevice) {
    m_count = 0;

    uint32_t page_bytes = sizeof(rm::device_info_table_params);
    auto* page = static_cast<rm::device_info_table_params*>(heap::uzalloc(page_bytes));
    if (page == nullptr) {
        log::error("nvidia: allocating %u bytes for the GSP firmware's device info table failed", page_bytes);
        return ERR_NO_MEMORY;
    }

    uint32_t count = 0;
    bool has_more = true;
    int32_t rc = OK;
    for (uint32_t first = 0; rc == OK && has_more; first += rm::DEVICE_INFO_PAGE_ENTRIES) {
        if (first >= rm::DEVICE_INFO_ENTRIES_MAX) {
            log::error(
                "nvidia: the GSP firmware's device info table runs past the %u entries it may hold",
                rm::DEVICE_INFO_ENTRIES_MAX
            );
            rc = ERR_BAD_MESSAGE;
            break;
        }

        string::memset(page, 0, page_bytes);
        page->first_entry = first;
        rc = fw.control(subdevice, rm::CONTROL_GET_DEVICE_INFO_TABLE, page, page_bytes);
        if (rc == OK) {
            rc = keep_page(*page, m_engines, count);
            has_more = page->has_more != 0;
        }
    }

    heap::ufree(page);

    if (rc == OK) {
        m_count = count;
    }

    return rc;
}

const engine_info* engine_table::first_async_copy_engine() const {
    const engine_info* graphics = nullptr;
    for (uint32_t i = 0; i < m_count; i++) {
        if (m_engines[i].type == rm::ENGINE_TYPE_GRAPHICS) {
            graphics = &m_engines[i];
        }
    }

    const engine_info* found = nullptr;
    for (uint32_t i = 0; i < m_count; i++) {
        const engine_info& engine = m_engines[i];
        bool is_async = graphics == nullptr || engine.runlist != graphics->runlist;
        bool is_lower = found == nullptr || engine.type < found->type;
        if (is_copy_engine(engine.type) && is_async && is_lower) {
            found = &engine;
        }
    }

    return found;
}

uint32_t engine_table::count() const {
    return m_count;
}

} // namespace drivers::nvidia
