#include <boot/legacy_memory_map.h>
#include <serial/serial.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/**
 * @brief Constructs a legacy memory map from the provided multiboot tag.
 * @param mmap_tag Pointer to the multiboot memory map tag.
 * 
 * Initializes the legacy memory map using the data provided in the multiboot tag.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
legacy_memory_map::legacy_memory_map(multiboot_tag_mmap* mmap_tag)
    : m_mmap_tag(mmap_tag),
      m_total_system_memory(0),
      m_total_conventional_memory(0),
      m_highest_address(0)
{
    if (!mmap_tag) {
        return;
    }

    // Each entry in the multiboot memory map has a known size
    m_entry_size = m_mmap_tag->entry_size;

    // Calculate the number of memory map entries
    uint32_t mmap_size = m_mmap_tag->size - sizeof(multiboot_tag_mmap);
    m_num_entries = mmap_size / m_entry_size;

    // Initialize the largest conventional segment
    m_largest_conventional_segment.base_addr = 0;
    m_largest_conventional_segment.length = 0;
    m_largest_conventional_segment.mem_available = false;

    // Iterate over all entries to compute stats
    for (uint32_t i = 0; i < m_num_entries; ++i) {
        auto* entry = reinterpret_cast<multiboot_mmap_entry*>(
            reinterpret_cast<uint8_t*>(m_mmap_tag->entries) + i * m_entry_size
        );

        // Compute base address and length from low/high parts
        uint64_t base_addr = (static_cast<uint64_t>(entry->base_addr_high) << 32) | entry->base_addr_low;
        uint64_t length    = (static_cast<uint64_t>(entry->length_high)    << 32) | entry->length_low;
        uint64_t region_end = base_addr + length;

        // Update highest address seen
        if (region_end > m_highest_address) {
            m_highest_address = region_end;
        }

        // Check if this region is conventional (available) memory
        if (entry->type == MULTIBOOT_MEMORY_TYPE_AVAILABLE) {
            m_total_system_memory += length;

            // Accumulate total conventional memory
            m_total_conventional_memory += length;

            // Track largest conventional memory segment
            if (length > m_largest_conventional_segment.length) {
                m_largest_conventional_segment.base_addr = base_addr;
                m_largest_conventional_segment.length = length;
                m_largest_conventional_segment.mem_available = true;
            }
        }
    }
}

/**
 * @brief Constructs an iterator for the legacy memory map.
 * @param current_entry Pointer to the current multiboot memory entry.
 * @param entry_size Size of each multiboot memory entry.
 * @param index Current index in the memory map.
 * @param num_entries Total number of entries in the memory map.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
legacy_memory_map::iterator::iterator(
    multiboot_mmap_entry* current_entry,
    uint32_t entry_size,
    uint32_t index,
    uint32_t num_entries
)
    : m_current(current_entry)
    , m_entry_size(entry_size)
    , m_index(index)
    , m_num_entries(num_entries)
{
}

/**
 * @brief Compares two iterators for inequality.
 * @param other The iterator to compare with.
 * @return True if the iterators are not equal, false otherwise.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
bool legacy_memory_map::iterator::operator!=(const iterator& other) const {
    return m_index != other.m_index;
}

/**
 * @brief Dereferences the iterator to access the current memory descriptor (wrapped).
 * @return A wrapped memory descriptor representing the current memory segment.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
memory_map_descriptor legacy_memory_map::iterator::operator*() const {
    memory_map_descriptor wrapper;

    // Calculate physical base address
    uint64_t base_addr = (static_cast<uint64_t>(m_current->base_addr_high) << 32) | m_current->base_addr_low;
    // Calculate length in bytes
    uint64_t length = (static_cast<uint64_t>(m_current->length_high) << 32) | m_current->length_low;

    wrapper.base_addr  = base_addr;
    wrapper.length = length;
    wrapper.mem_available = m_current->type == MULTIBOOT_MEMORY_TYPE_AVAILABLE ||
                            m_current->type == MULTIBOOT_MEMORY_TYPE_ACPI_RECLAIMABLE;

    return wrapper;
}

/**
 * @brief Advances the iterator to the next memory descriptor.
 * @return Reference to the updated iterator.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
legacy_memory_map::iterator& legacy_memory_map::iterator::operator++() {
    m_current = reinterpret_cast<multiboot_mmap_entry*>(
        reinterpret_cast<uint8_t*>(m_current) + m_entry_size
    );
    ++m_index;
    return *this;
}

/**
 * @brief Retrieves an iterator to the beginning of the legacy memory map.
 * @return An iterator pointing to the first memory descriptor.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
legacy_memory_map::iterator legacy_memory_map::begin() const {
    auto* start = reinterpret_cast<multiboot_mmap_entry*>(m_mmap_tag->entries);
    return iterator(start, m_entry_size, 0, m_num_entries);
}

/**
 * @brief Retrieves an iterator to the end of the legacy memory map.
 * @return An iterator representing the end of the memory map.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
legacy_memory_map::iterator legacy_memory_map::end() const {
    // End iterator: index == m_num_entries, current pointer is irrelevant
    return iterator(nullptr, m_entry_size, m_num_entries, m_num_entries);
}

/**
 * @brief Retrieves the number of memory descriptors in the legacy memory map.
 * @return The total number of entries in the memory map.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
uint32_t legacy_memory_map::get_num_entries() const {
    return m_num_entries;
}

/**
 * @brief Computes the total system memory based on the legacy memory map.
 * @return The total amount of system memory in bytes.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
uint64_t legacy_memory_map::get_total_system_memory() const {
    return m_total_system_memory;
}

/**
 * @brief Computes the total amount of conventional memory (MULTIBOOT_MEMORY_TYPE_AVAILABLE).
 * @return The total amount of conventional memory in bytes.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
uint64_t legacy_memory_map::get_total_conventional_memory() const {
    return m_total_conventional_memory;
}

/**
 * @brief Retrieves the highest memory address in the legacy memory map.
 * @return The highest physical memory address in the map.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
uintptr_t legacy_memory_map::get_highest_address() const {
    return static_cast<uintptr_t>(m_highest_address);
}

/**
 * @brief Retrieves a memory descriptor at a specified index.
 * @param idx Index of the memory descriptor to retrieve.
 * @return The memory descriptor at the specified index.
 *
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
memory_map_descriptor legacy_memory_map::get_entry_desc(size_t idx) const {
    memory_map_descriptor desc;
    desc.mem_available = false;
    desc.base_addr = 0;
    desc.length = 0;

    if (idx >= m_num_entries) {
        // Out of range; return an unavailable descriptor
        return desc;
    }

    auto* entry = reinterpret_cast<multiboot_mmap_entry*>(
        reinterpret_cast<uint8_t*>(m_mmap_tag->entries) + idx * m_entry_size
    );

    uint64_t base_addr = (static_cast<uint64_t>(entry->base_addr_high) << 32) | entry->base_addr_low;
    uint64_t length    = (static_cast<uint64_t>(entry->length_high) << 32) | entry->length_low;

    desc.base_addr = base_addr;
    desc.length    = length;
    desc.mem_available = (entry->type == MULTIBOOT_MEMORY_TYPE_AVAILABLE);

    return desc;
}

/**
 * @brief Finds the largest segment of conventional memory (MULTIBOOT_MEMORY_TYPE_AVAILABLE).
 * @return A descriptor representing the largest conventional memory segment.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
memory_map_descriptor legacy_memory_map::get_largest_conventional_segment() const {
    return m_largest_conventional_segment;
}

/**
 * @brief Finds a memory segment suitable for a specified allocation block.
 * @param min_address Minimum address of the desired memory range.
 * @param max_address Maximum address of the desired memory range.
 * @param size Size of the allocation block in bytes.
 * @return A descriptor representing a suitable memory segment.
 * 
 * Searches for a memory segment within the specified range that meets the allocation size requirements.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
memory_map_descriptor legacy_memory_map::find_segment_for_allocation_block(
    uint64_t min_address,
    uint64_t max_address,
    uint64_t size
) const {
    memory_map_descriptor largest_segment;
    largest_segment.base_addr = 0;
    largest_segment.length = 0;
    largest_segment.mem_available = false;

    // Iterate through all descriptors
    for (uint32_t i = 0; i < m_num_entries; ++i) {
        auto* entry_ptr = reinterpret_cast<multiboot_mmap_entry*>(
            reinterpret_cast<uint8_t*>(m_mmap_tag->entries) + i * m_entry_size
        );

        // Only consider available memory
        if (entry_ptr->type != MULTIBOOT_MEMORY_TYPE_AVAILABLE) {
            continue;
        }

        uint64_t start  = (static_cast<uint64_t>(entry_ptr->base_addr_high) << 32) | entry_ptr->base_addr_low;
        uint64_t length = (static_cast<uint64_t>(entry_ptr->length_high) << 32) | entry_ptr->length_low;
        uint64_t end    = start + length;

        // Check if the segment overlaps with the specified range
        if (end <= min_address || start >= max_address) {
            continue;
        }

        // Clip the segment to the specified range
        uint64_t clipped_start = MAX(start, min_address);
        uint64_t clipped_end   = MIN(end, max_address);
        uint64_t clipped_length = (clipped_end > clipped_start) ? (clipped_end - clipped_start) : 0;

        // Check if the clipped segment can fit the requested size
        if (clipped_length >= size && clipped_length > largest_segment.length) {
            largest_segment.base_addr = clipped_start;
            largest_segment.length = clipped_length;
            largest_segment.mem_available = true;
        }
    }

    return largest_segment;
}

/**
 * @brief Prints the legacy memory map to the console or log.
 * 
 * Outputs detailed information about the legacy memory map for debugging or analysis purposes.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
void legacy_memory_map::print_memory_map() {
    serial::printf("Legacy Memory Map:\n");
    serial::printf("  Total System Memory: %llu KB\n", get_total_system_memory() / 1024);
    serial::printf("  Total Conventional Memory: %llu KB\n\n", get_total_conventional_memory() / 1024);

    for (auto it = begin(); it != end(); ++it) {
        auto entry = *it;

        // Filter for available memory
        if (!entry.mem_available) {
            continue;
        }

        uint64_t physical_start = entry.base_addr;
        uint64_t length         = entry.length;
        uint64_t physical_end   = physical_start + length;
        uint64_t size_kb        = length / 1024;
        uint64_t size_mb        = size_kb / 1024;
        uint64_t pages          = length / 4096; // 4KB pages

        serial::printf(
            "  Available: %u, Size: %llu MB (%llu pages)\n"
            "  Physical: 0x%016llx - 0x%016llx\n",
            entry.mem_available,
            size_mb,
            pages,
            physical_start,
            physical_end
        );
    }

    serial::printf("\n");
}
