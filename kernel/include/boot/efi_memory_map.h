#ifndef EFIMEM_H
#define EFIMEM_H
#include "multiboot2.h"
#include "boot_memory_map.h"

#define EFI_MEMORY_TYPE_RESERVED_MEMORY         0
#define EFI_MEMORY_TYPE_LOADER_CODE             1
#define EFI_MEMORY_TYPE_LOADER_DATA             2
#define EFI_MEMORY_TYPE_BOOT_SERVICES_CODE      3
#define EFI_MEMORY_TYPE_BOOT_SERVICES_DATA      4
#define EFI_MEMORY_TYPE_RUNTIME_SERVICES_CODE   5
#define EFI_MEMORY_TYPE_RUNTIME_SERVICES_DATA   6
#define EFI_MEMORY_TYPE_CONVENTIONAL_MEMORY     7
#define EFI_MEMORY_TYPE_UNUSABLE_MEMORY         8
#define EFI_MEMORY_TYPE_ACPI_RECLAIM_MEMORY     9
#define EFI_MEMORY_TYPE_ACPI_MEMORY_NVS         10
#define EFI_MEMORY_TYPE_MEMORY_MAPPED_IO        11
#define EFI_MEMORY_TYPE_MEMORY_MAPPED_IO_PORT   12
#define EFI_MEMORY_TYPE_PAL_CODE                13
#define EFI_MEMORY_TYPE_PERSISTENT_MEMORY       14
#define EFI_MEMORY_TYPE_MAX_MEMORY_TYPE         15

struct efi_memory_descriptor {
    multiboot_uint32_t type;
    multiboot_uint32_t reserved;
    multiboot_uint64_t physical_start;
    multiboot_uint64_t virtual_start;
    multiboot_uint64_t page_count;
    multiboot_uint64_t attribute;
};

struct efi_memory_descriptor_wrapper {
    efi_memory_descriptor* desc;
    uint64_t paddr;
    uint64_t length;
};

/**
 * @class efi_memory_map
 * @brief Represents and provides utilities for interacting with the EFI memory map.
 * 
 * This class encapsulates the EFI memory map, providing functionality to iterate over memory descriptors,
 * query memory statistics, and locate specific memory regions.
 */
class efi_memory_map : public boot_memory_map {
public:
    /**
     * @brief Constructs an EFI memory map from the provided multiboot tag.
     * @param efi_mmap_tag Pointer to the multiboot EFI memory map tag.
     * 
     * Initializes the EFI memory map using the data provided in the multiboot tag.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE efi_memory_map(multiboot_tag_efi_mmap* efi_mmap_tag);

    /**
     * @class iterator
     * @brief Iterator class for traversing EFI memory descriptors.
     * 
     * Provides range-based for loop compatibility for traversing the EFI memory map.
     */
    class iterator {
    public:
        /**
         * @brief Constructs an iterator for the EFI memory map.
         * @param current Pointer to the current memory descriptor.
         * @param descr_size Size of each memory descriptor.
         * @param index Current index in the memory map.
         * @param num_entries Total number of entries in the memory map.
         * 
         * Initializes the iterator with the current position and descriptor size.
         * 
         * @note Privilege: **required**
         */
        __PRIVILEGED_CODE iterator(uint8_t* current, uint32_t descr_size, uint32_t index, uint32_t num_entries);

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
         * @brief Dereferences the iterator to access the current memory descriptor.
         * @return A wrapped memory descriptor representing the current memory segment.
         * 
         * Provides access to the memory descriptor at the iterator's current position.
         * 
         * @note Privilege: **required**
         */
        __PRIVILEGED_CODE efi_memory_descriptor_wrapper operator*() const;

        /**
         * @brief Advances the iterator to the next memory descriptor.
         * @return Reference to the updated iterator.
         * 
         * Moves the iterator to the next entry in the EFI memory map.
         * 
         * @note Privilege: **required**
         */
        __PRIVILEGED_CODE iterator& operator++();

    private:
        uint8_t* m_current;   /** Pointer to the current memory descriptor */
        uint32_t m_descr_size; /** Size of each memory descriptor */
        uint32_t m_index;      /** Current index in the memory map */
        uint32_t m_num_entries; /** Total number of entries in the memory map */
    };

    /**
     * @brief Retrieves an iterator to the beginning of the EFI memory map.
     * @return An iterator pointing to the first memory descriptor.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE iterator begin() const;

    /**
     * @brief Retrieves an iterator to the end of the EFI memory map.
     * @return An iterator representing the end of the memory map.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE iterator end() const;

    /**
     * @brief Retrieves the number of memory descriptors in the EFI memory map.
     * @return The total number of entries in the memory map.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint32_t get_num_entries() const override;

    /**
     * @brief Computes the total system memory based on the EFI memory map.
     * @return The total amount of system memory in bytes.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint64_t get_total_system_memory() const override;

    /**
     * @brief Computes the total amount of conventional memory.
     * @return The total amount of conventional memory in bytes.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint64_t get_total_conventional_memory() const override;

    /**
     * @brief Retrieves the highest memory address in the EFI memory map.
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
     * @brief Finds the largest segment of conventional memory.
     * @return A wrapped descriptor representing the largest conventional memory segment.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE memory_map_descriptor get_largest_conventional_segment() const override;

    /**
     * @brief Finds a memory segment suitable for a specified allocation block.
     * @param min_address Minimum address of the desired memory range.
     * @param max_address Maximum address of the desired memory range.
     * @param size Size of the allocation block in bytes.
     * @return A wrapped descriptor representing a suitable memory segment.
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
     * @brief Prints the EFI memory map to the console or log.
     * 
     * Outputs detailed information about the EFI memory map for debugging or analysis purposes.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void print_memory_map() override;

private:
    multiboot_tag_efi_mmap* m_efi_mmap_tag; /** Pointer to the EFI memory map tag from multiboot */
    uint32_t m_descr_size;                 /** Size of each memory descriptor */
    uint32_t m_num_entries;                /** Total number of memory descriptors in the map */
    uint64_t m_total_system_memory;        /** Total amount of system memory in bytes */
    uint64_t m_total_conventional_memory;  /** Total amount of conventional memory in bytes */
    uint64_t m_highest_address;            /** Highest physical memory address in the map */
    efi_memory_descriptor_wrapper m_largest_conventional_segment; /** Largest conventional memory segment */
};

#endif // EFIMEM_H
