// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/vram_allocator.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"

namespace drivers::nvidia {

// The GPU's small page, the unit of every range's offset and size
constexpr uint64_t PAGE_BYTES = 0x1000;

// Rounds `value` up to a multiple of `alignment`, a power of two, refusing a result past 2^64 - 1
static bool round_up(uint64_t value, uint64_t alignment, uint64_t& out) {
    uint64_t mask = alignment - 1;
    bool wraps = value + mask < value;
    if (wraps) {
        return false;
    }

    out = (value + mask) & ~mask;
    return true;
}

static uint64_t last_byte_of(const video_memory_region& region) {
    return region.offset + region.size - 1;
}

// Whether the `bytes` bytes at `offset` lie wholly inside `region`
static bool holds(const video_memory_region& region, uint64_t offset, uint64_t bytes) {
    return offset >= region.offset && lies_inside(offset - region.offset, bytes, region.size);
}

// Whether an allocation for `use` may lie in `region`, a usable region not for protected allocations, with no default
// case so the compiler flags a use added to `vram_use` but missing here
static bool may_lie_in(vram_use use, const video_memory_region& region) {
    switch (use) {
        case vram_use::general:
            return true;
        case vram_use::scanout:
            return region.supports_scanout;
    }
}

// Logs and refuses a table whose regions overlap, whose usable regions end past video memory or in which no reserved
// region wholly holds BAR1's root entry
static int32_t check_regions(const static_configuration& configuration, uint64_t video_memory_size) {
    const video_memory_region* regions = configuration.regions;
    uint32_t count = configuration.region_count;

    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = i + 1; j < count; j++) {
            const video_memory_region& region = regions[i];
            const video_memory_region& other = regions[j];
            bool overlaps = region.offset <= last_byte_of(other) && other.offset <= last_byte_of(region);
            if (overlaps) {
                log::error(
                    "nvidia: video memory regions %u (0x%lX to 0x%lX) and %u (0x%lX to 0x%lX) overlap",
                    i,
                    region.offset,
                    last_byte_of(region),
                    j,
                    other.offset,
                    last_byte_of(other)
                );
                return ERR_BAD_MESSAGE;
            }
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        const video_memory_region& region = regions[i];
        bool ends_past_video_memory = !lies_inside(region.offset, region.size, video_memory_size);
        if (!region.is_reserved && ends_past_video_memory) {
            log::error(
                "nvidia: usable video memory region %u (0x%lX to 0x%lX) ends past the 0x%lX bytes of video memory",
                i,
                region.offset,
                last_byte_of(region),
                video_memory_size
            );
            return ERR_BAD_MESSAGE;
        }
    }

    uint64_t root_entry_offset = configuration.bar1_page_directory_offset;
    for (uint32_t i = 0; i < count; i++) {
        if (regions[i].is_reserved && holds(regions[i], root_entry_offset, sizeof(uint64_t))) {
            return OK;
        }
    }

    log::error(
        "nvidia: no reserved video memory region holds the first entry of BAR1's root page directory, at 0x%lX in "
        "video memory",
        root_entry_offset
    );
    return ERR_BAD_MESSAGE;
}

vram_allocator::vram_allocator() {
    m_records.init();
    m_lock.init();
}

vram_allocator::~vram_allocator() {
    record* left = m_records.pop_front();
    while (left != nullptr) {
        heap::ufree_delete(left);
        left = m_records.pop_front();
    }
}

int32_t vram_allocator::init(const static_configuration& configuration, uint64_t video_memory_size) {
    int32_t rc = check_regions(configuration, video_memory_size);
    if (rc != OK) {
        return rc;
    }

    RUN_ELEVATED(sync::mutex_lock(m_lock));

    // A second call, which the contract forbids, replaces the regions instead of writing past the array
    m_region_count = 0;

    // A region moves only past regions of lower performance, so regions of equal performance keep the firmware's order
    for (uint32_t i = 0; i < configuration.region_count; i++) {
        const video_memory_region& region = configuration.regions[i];
        if (region.is_reserved || region.is_protected) {
            continue;
        }

        uint32_t slot = m_region_count;
        while (slot > 0 && m_regions[slot - 1].performance < region.performance) {
            m_regions[slot] = m_regions[slot - 1];
            slot--;
        }

        m_regions[slot] = region;
        m_region_count++;
    }

    RUN_ELEVATED(sync::mutex_unlock(m_lock));

    return OK;
}

int32_t vram_allocator::allocate(uint64_t size, uint64_t alignment, vram_use use, vram_range& out) {
    uint64_t rounded_size = 0;
    bool is_size_valid = size != 0 && round_up(size, PAGE_BYTES, rounded_size);
    if (!is_size_valid) {
        log::error(
            "nvidia: a video memory allocation of 0x%lX bytes was refused, as the size is 0 or overflows when rounded "
            "to whole pages",
            size
        );
        return ERR_INVALID_ARG;
    }

    bool is_alignment_valid = alignment != 0 && (alignment & (alignment - 1)) == 0;
    if (!is_alignment_valid) {
        log::error(
            "nvidia: a video memory allocation at alignment 0x%lX was refused, as the alignment is not a power of two",
            alignment
        );
        return ERR_INVALID_ARG;
    }

    uint64_t rounded_alignment = alignment > PAGE_BYTES ? alignment : PAGE_BYTES;

    RUN_ELEVATED(sync::mutex_lock(m_lock));

    int32_t rc = place_range(rounded_size, rounded_alignment, use, out);

    RUN_ELEVATED(sync::mutex_unlock(m_lock));

    return rc;
}

void vram_allocator::release(const vram_range& range) {
    RUN_ELEVATED(sync::mutex_lock(m_lock));

    record* released = find_record(range);
    if (released != nullptr) {
        m_records.remove(released);
    }

    RUN_ELEVATED(sync::mutex_unlock(m_lock));

    if (released == nullptr) {
        log::error(
            "nvidia: releasing 0x%lX bytes of video memory at 0x%lX changes nothing, as it matches no allocation",
            range.size,
            range.offset
        );
        return;
    }

    heap::ufree_delete(released);
}

int32_t vram_allocator::place_range(uint64_t size, uint64_t alignment, vram_use use, vram_range& out) {
    uint64_t offset = 0;
    bool is_found = false;
    for (uint32_t i = 0; i < m_region_count && !is_found; i++) {
        is_found = may_lie_in(use, m_regions[i]) && find_free_offset(m_regions[i], size, alignment, offset);
    }

    if (!is_found) {
        return ERR_NO_MEMORY;
    }

    record* added = heap::ualloc_new<record>();
    if (added == nullptr) {
        log::error(
            "nvidia: allocating the record of 0x%lX bytes of video memory at 0x%lX failed, so they are not handed out",
            size,
            offset
        );
        return ERR_NO_MEMORY;
    }

    added->offset = offset;
    added->size = size;
    m_records.insert_sorted(added, [](const record* a, const record* b) {
        return a->offset < b->offset;
    });

    out = {.offset = offset, .size = size};

    return OK;
}

bool vram_allocator::find_free_offset(
    const video_memory_region& region,
    uint64_t size,
    uint64_t alignment,
    uint64_t& offset_out
) {
    // `init` keeps only regions ending at or below video memory's size, so no region's or record's end wraps
    uint64_t region_end = region.offset + region.size;
    uint64_t candidate = 0;
    if (!round_up(region.offset, alignment, candidate)) {
        return false;
    }

    for (const record& held : m_records) {
        uint64_t held_end = held.offset + held.size;
        if (held_end <= candidate) {
            continue;
        }

        if (!lies_inside(candidate, size, region_end)) {
            return false;
        }

        if (candidate + size <= held.offset) {
            break;
        }

        if (!round_up(held_end, alignment, candidate)) {
            return false;
        }
    }

    if (!lies_inside(candidate, size, region_end)) {
        return false;
    }

    offset_out = candidate;
    return true;
}

vram_allocator::record* vram_allocator::find_record(const vram_range& range) {
    for (record& held : m_records) {
        if (held.offset == range.offset && held.size == range.size) {
            return &held;
        }
    }

    return nullptr;
}

} // namespace drivers::nvidia
