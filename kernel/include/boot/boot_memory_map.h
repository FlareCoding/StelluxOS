#ifndef BOOT_MEMORY_MAP_H
#define BOOT_MEMORY_MAP_H
#include <types.h>

/**
 * @struct memory_map_descriptor
 * @brief Represents a unified memory descriptor for EFI and legacy memory maps.
 */
struct memory_map_descriptor {
    uint64_t base_addr;   /**< Physical start address of the memory region */
    uint64_t length;      /**< Length of the memory region in bytes */
    bool mem_available;
};

/**
 * @class memory_map
 * @brief Abstract interface for system boot-time memory maps.
 *
 * Provides a common interface for retrieving and iterating over memory descriptors,
 * regardless of whether the system uses EFI or legacy memory mapping.
 */
class boot_memory_map {
public:
    virtual ~boot_memory_map() = default;

    /**
     * @brief Retrieves the number of memory descriptors in the memory map.
     * @return The total number of entries in the memory map.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual uint32_t get_num_entries() const = 0;

    /**
     * @brief Computes the total system memory based on the memory map.
     * @return The total amount of system memory in bytes.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual uint64_t get_total_system_memory() const = 0;

    /**
     * @brief Computes the total amount of conventional memory.
     * @return The total amount of conventional memory in bytes.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual uint64_t get_total_conventional_memory() const = 0;

    /**
     * @brief Retrieves the highest memory address in the memory map.
     * @return The highest physical memory address in the map.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual uintptr_t get_highest_address() const = 0;

    /**
     * @brief Retrieves a memory descriptor at a specified index.
     * @param idx Index of the memory descriptor to retrieve.
     * @return The memory descriptor at the specified index.
     *
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual memory_map_descriptor get_entry_desc(size_t idx) const = 0;

    /**
     * @brief Finds the largest segment of conventional memory.
     * @return A wrapper descriptor representing the largest conventional memory segment.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual memory_map_descriptor get_largest_conventional_segment() const = 0;

    /**
     * @brief Finds a memory segment suitable for a specified allocation block.
     * @param min_address Minimum address of the desired memory range.
     * @param max_address Maximum address of the desired memory range.
     * @param size Size of the allocation block in bytes.
     * @return A wrapper descriptor representing a suitable memory segment.
     * 
     * Searches for a memory segment within the specified range that meets the allocation size requirements.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual memory_map_descriptor find_segment_for_allocation_block(
        uint64_t min_address,
        uint64_t max_address,
        uint64_t size
    ) const = 0;

    /**
     * @brief Prints the EFI memory map to the console or log.
     * 
     * Outputs detailed information about the memory map for debugging or analysis purposes.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual void print_memory_map() = 0;
};

#endif // BOOT_MEMORY_MAP_H