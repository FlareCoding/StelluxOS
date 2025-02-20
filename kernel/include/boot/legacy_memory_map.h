#ifndef LEGACY_MEMORY_MAP_H
#define LEGACY_MEMORY_MAP_H
#include "boot_memory_map.h"
#include "multiboot2.h"

/**
 * @class legacy_memory_map
 * @brief Represents and provides utilities for interacting with the legacy (multiboot) memory map.
 * 
 * This class encapsulates the legacy multiboot memory map, providing functionality to iterate over 
 * memory descriptors, query memory statistics, and locate specific memory regions.
 */
class legacy_memory_map : public boot_memory_map {
public:
    /**
     * @brief Constructs a legacy memory map from the provided multiboot tag.
     * @param mmap_tag Pointer to the multiboot memory map tag.
     * 
     * Initializes the legacy memory map using the data provided in the multiboot tag.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE explicit legacy_memory_map(multiboot_tag_mmap* mmap_tag);

    /**
     * @class iterator
     * @brief Iterator class for traversing legacy multiboot memory descriptors.
     * 
     * Provides range-based for loop compatibility for traversing the legacy memory map.
     */
    class iterator {
    public:
        /**
         * @brief Constructs an iterator for the legacy memory map.
         * @param current_entry Pointer to the current multiboot memory entry.
         * @param entry_size Size of each multiboot memory entry.
         * @param index Current index in the memory map.
         * @param num_entries Total number of entries in the memory map.
         * 
         * @note Privilege: **required**
         */
        __PRIVILEGED_CODE iterator(multiboot_mmap_entry* current_entry, uint32_t entry_size, uint32_t index, uint32_t num_entries);

        /**
         * @brief Compares two iterators for inequality.
         * @param other The iterator to compare with.
         * @return True if the iterators are not equal, false otherwise.
         * 
         * Used for determining the end of iteration.
         * 
         * @note Privilege: **required**
         */
        __PRIVILEGED_CODE bool operator!=(const iterator& other) const;

        /**
         * @brief Dereferences the iterator to access the current memory descriptor (wrapped).
         * @return A wrapped memory descriptor representing the current memory segment.
         * 
         * @note Privilege: **required**
         */
        __PRIVILEGED_CODE memory_map_descriptor operator*() const;

        /**
         * @brief Advances the iterator to the next memory descriptor.
         * @return Reference to the updated iterator.
         * 
         * @note Privilege: **required**
         */
        __PRIVILEGED_CODE iterator& operator++();

    private:
        multiboot_mmap_entry* m_current; /**< Pointer to the current multiboot memory entry */
        uint32_t m_entry_size;           /**< Size of each multiboot memory entry           */
        uint32_t m_index;                /**< Current index in the memory map               */
        uint32_t m_num_entries;          /**< Total number of entries in the memory map     */
    };

    /**
     * @brief Retrieves an iterator to the beginning of the legacy memory map.
     * @return An iterator pointing to the first memory descriptor.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE iterator begin() const;

    /**
     * @brief Retrieves an iterator to the end of the legacy memory map.
     * @return An iterator representing the end of the memory map.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE iterator end() const;

    /**
     * @brief Retrieves the number of memory descriptors in the legacy memory map.
     * @return The total number of entries in the memory map.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint32_t get_num_entries() const override;

    /**
     * @brief Computes the total system memory based on the legacy memory map.
     * @return The total amount of system memory in bytes.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint64_t get_total_system_memory() const override;

    /**
     * @brief Computes the total amount of conventional memory (MULTIBOOT_MEMORY_TYPE_AVAILABLE).
     * @return The total amount of conventional memory in bytes.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint64_t get_total_conventional_memory() const override;

    /**
     * @brief Retrieves the highest memory address in the legacy memory map.
     * @return The highest physical memory address in the map.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uintptr_t get_highest_address() const override;

    /**
     * @brief Retrieves a memory descriptor at a specified index.
     * @param idx Index of the memory descriptor to retrieve.
     * @return The memory descriptor at the specified index.
     *
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE memory_map_descriptor get_entry_desc(size_t idx) const override;

    /**
     * @brief Finds the largest segment of conventional memory (MULTIBOOT_MEMORY_TYPE_AVAILABLE).
     * @return A descriptor representing the largest conventional memory segment.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE memory_map_descriptor get_largest_conventional_segment() const override;

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
    __PRIVILEGED_CODE memory_map_descriptor find_segment_for_allocation_block(
        uint64_t min_address,
        uint64_t max_address,
        uint64_t size
    ) const override;

    /**
     * @brief Prints the legacy memory map to the console or log.
     * 
     * Outputs detailed information about the legacy memory map for debugging or analysis purposes.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void print_memory_map() override;

private:
    multiboot_tag_mmap* m_mmap_tag;            /**< Pointer to the multiboot memory map tag */
    uint32_t m_entry_size;                     /**< Size of each multiboot memory entry     */
    uint32_t m_num_entries;                    /**< Total number of memory descriptors      */
    uint64_t m_total_system_memory;            /**< Total amount of system memory in bytes  */
    uint64_t m_total_conventional_memory;      /**< Total amount of conventional memory     */
    uint64_t m_highest_address;                /**< Highest physical memory address         */

    /**
     * @brief Tracks the largest conventional memory segment.
     *
     * This is for quick lookup of the single largest chunk of type MULTIBOOT_MEMORY_TYPE_AVAILABLE.
     */
    memory_map_descriptor m_largest_conventional_segment;
};

#endif // LEGACY_MEMORY_MAP_H
